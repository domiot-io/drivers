#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/time.h>
#include <linux/rtc.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/mutex.h>

#define DEVICE_NAME "lcd-sim"
#define CLASS_NAME "lcd"
#define MAX_DEVICES 10
#define MAX_LOG_ENTRIES 30
#define LCD_MAX_CHARS 120
#define LOG_ENTRY_SIZE 256

// compatibility macros
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)
    #define CLASS_CREATE_COMPAT(name) class_create(name)
#else
    #define CLASS_CREATE_COMPAT(name) class_create(THIS_MODULE, name)
#endif

// debug: 0=errors only, 1=+init/cleanup, 2=+operations, 3=+verbose
static int debug_level = 1;
module_param(debug_level, int, 0644);
MODULE_PARM_DESC(debug_level, "Debug level: 0=errors, 1=init/cleanup, 2=operations, 3=verbose (default: 1)");

static int num_devices = 1;
module_param(num_devices, int, S_IRUGO);
MODULE_PARM_DESC(num_devices, "Number of LCD devices to create (default: 1, max: 10)");

// debug macros to reduce overhead
#define dbg_err(fmt, ...) printk(KERN_ERR "lcd-sim: " fmt, ##__VA_ARGS__)
#define dbg_info(level, fmt, ...) do { if (debug_level >= level) printk(KERN_INFO "lcd-sim: " fmt, ##__VA_ARGS__); } while(0)
#define dbg_dev_info(level, dev_id, fmt, ...) do { if (debug_level >= level) printk(KERN_INFO "lcd-sim%d: " fmt, dev_id, ##__VA_ARGS__); } while(0)

struct lcd_device {
    dev_t dev_num;
    struct cdev cdev;
    struct device *device;
    int minor;
    char current_text[LCD_MAX_CHARS + 1];
    char log_entries[MAX_LOG_ENTRIES][LOG_ENTRY_SIZE];
    int log_count;
    int log_head;
    struct mutex log_mutex;
    struct mutex text_mutex;
};

static int major_number;
static struct class *lcd_class = NULL;
static struct lcd_device *devices = NULL;

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);
static void add_log_entry(struct lcd_device *dev, const char *text);
static void write_log_to_file(struct lcd_device *dev);

static struct file_operations fops = {
    .open = device_open,
    .write = device_write,
    .release = device_release,
};

static int device_open(struct inode *inodep, struct file *filep)
{
    int minor = iminor(inodep);
    if (minor >= num_devices) {
        dbg_err("Invalid minor number %d\n", minor);
        return -ENODEV;
    }
    
    filep->private_data = &devices[minor];
    dbg_dev_info(2, minor, "LCD device opened\n");
    return 0;
}

static ssize_t device_write(struct file *filep, const char *buffer, size_t len, loff_t *offset)
{
    struct lcd_device *dev = (struct lcd_device *)filep->private_data;
    char *user_input = NULL;
    char processed_text[LCD_MAX_CHARS + 1];
    int i, processed_len = 0;
    
    if (!dev) {
        dbg_err("Invalid device pointer\n");
        return -EFAULT;
    }
    
    if (len == 0) {
        return 0;
    }
    
    user_input = kmalloc(len + 1, GFP_KERNEL);
    if (!user_input) {
        dbg_err("Failed to allocate memory for user input\n");
        return -ENOMEM;
    }
    
    if (copy_from_user(user_input, buffer, len)) {
        kfree(user_input);
        return -EFAULT;
    }
    user_input[len] = '\0';
    
    // input text: take first 120 characters, convert newlines to spaces
    memset(processed_text, 0, sizeof(processed_text));
    for (i = 0; i < len && processed_len < LCD_MAX_CHARS; i++) {
        if (user_input[i] == '\n' || user_input[i] == '\r') {
            processed_text[processed_len] = ' ';
            processed_len++;
        } else if (user_input[i] >= 32 && user_input[i] <= 126) {
            processed_text[processed_len] = user_input[i];
            processed_len++;
        }
    }
    processed_text[processed_len] = '\0';
    
    mutex_lock(&dev->text_mutex);
    strncpy(dev->current_text, processed_text, LCD_MAX_CHARS);
    dev->current_text[LCD_MAX_CHARS] = '\0';
    mutex_unlock(&dev->text_mutex);
    
    add_log_entry(dev, processed_text);
    write_log_to_file(dev);
    
    dbg_dev_info(2, dev->minor, "LCD updated with text: \"%s\" (%d chars)\n", 
                 processed_text, processed_len);
    
    kfree(user_input);
    return len;
}

static int device_release(struct inode *inodep, struct file *filep)
{
    struct lcd_device *dev = (struct lcd_device *)filep->private_data;
    if (dev) {
        dbg_dev_info(2, dev->minor, "LCD device closed\n");
    }
    return 0;
}

static void add_log_entry(struct lcd_device *dev, const char *text)
{
    struct timespec64 ts;
    struct tm tm;
    
    mutex_lock(&dev->log_mutex);
    
    ktime_get_real_ts64(&ts);
    time64_to_tm(ts.tv_sec, 0, &tm);
    
    // Format: "YYYY-MM-DD HH:MM:SS <text>"
    snprintf(dev->log_entries[dev->log_head], LOG_ENTRY_SIZE,
             "%04ld-%02d-%02d %02d:%02d:%02d %s",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, text);
    
    dev->log_head = (dev->log_head + 1) % MAX_LOG_ENTRIES;
    if (dev->log_count < MAX_LOG_ENTRIES) {
        dev->log_count++;
    }
    
    mutex_unlock(&dev->log_mutex);
}

static void write_log_to_file(struct lcd_device *dev)
{
    struct file *file;
    char filename[32];
    int i, idx;
    loff_t pos = 0;
    
    snprintf(filename, sizeof(filename), "/tmp/lcd-output%d", dev->minor);
    
    file = filp_open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (IS_ERR(file)) {
        dbg_err("Failed to open log file %s\n", filename);
        return;
    }
    
    mutex_lock(&dev->log_mutex);
    
    // entries in reverse order (newest first)
    for (i = 0; i < dev->log_count; i++) {
        idx = (dev->log_head - 1 - i + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
        kernel_write(file, dev->log_entries[idx], strlen(dev->log_entries[idx]), &pos);
        kernel_write(file, "\n", 1, &pos);
    }
    
    mutex_unlock(&dev->log_mutex);
    
    filp_close(file, NULL);
    
    dbg_dev_info(3, dev->minor, "Log written to %s\n", filename);
}

static int __init lcd_init(void)
{
    int i, result;
    
    if (num_devices <= 0 || num_devices > MAX_DEVICES) {
        dbg_err("Invalid number of devices: %d (must be 1-%d)\n", num_devices, MAX_DEVICES);
        return -EINVAL;
    }
    
    dbg_info(1, "Initializing %d LCD device(s)\n", num_devices);
    
    result = alloc_chrdev_region(&major_number, 0, num_devices, DEVICE_NAME);
    if (result < 0) {
        dbg_err("Failed to allocate major number\n");
        return result;
    }
    major_number = MAJOR(major_number);
    dbg_info(1, "Registered correctly with major number %d\n", major_number);
    
    lcd_class = CLASS_CREATE_COMPAT(CLASS_NAME);
    if (IS_ERR(lcd_class)) {
        unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
        dbg_err("Failed to create device class\n");
        return PTR_ERR(lcd_class);
    }
    dbg_info(1, "Device class created correctly\n");
    
    devices = kcalloc(num_devices, sizeof(struct lcd_device), GFP_KERNEL);
    if (!devices) {
        class_destroy(lcd_class);
        unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
        dbg_err("Failed to allocate memory for devices\n");
        return -ENOMEM;
    }
    
    for (i = 0; i < num_devices; i++) {
        devices[i].dev_num = MKDEV(major_number, i);
        devices[i].minor = i;
        devices[i].log_count = 0;
        devices[i].log_head = 0;
        mutex_init(&devices[i].log_mutex);
        mutex_init(&devices[i].text_mutex);
        
        // initialize LCD with empty text
        memset(devices[i].current_text, 0, sizeof(devices[i].current_text));
        
        cdev_init(&devices[i].cdev, &fops);
        devices[i].cdev.owner = THIS_MODULE;
        
        result = cdev_add(&devices[i].cdev, devices[i].dev_num, 1);
        if (result) {
            dbg_err("Failed to add cdev for device %d\n", i);
            goto cleanup_devices;
        }
        
        devices[i].device = device_create(lcd_class, NULL, devices[i].dev_num, NULL, DEVICE_NAME "%d", i);
        if (IS_ERR(devices[i].device)) {
            dbg_err("Failed to create device %d\n", i);
            cdev_del(&devices[i].cdev);
            result = PTR_ERR(devices[i].device);
            goto cleanup_devices;
        }
        
        dbg_dev_info(1, i, "LCD device created correctly\n");
        dbg_dev_info(2, i, "LCD initialized with empty display\n");
    }
    
    dbg_info(1, "Module loaded successfully\n");
    return 0;
    
cleanup_devices:
    for (i--; i >= 0; i--) {
        device_destroy(lcd_class, devices[i].dev_num);
        cdev_del(&devices[i].cdev);
        mutex_destroy(&devices[i].log_mutex);
        mutex_destroy(&devices[i].text_mutex);
    }
    kfree(devices);
    class_destroy(lcd_class);
    unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
    return result;
}

static void __exit lcd_exit(void)
{
    int i;
    
    dbg_info(1, "Unloading LCD module\n");
    
    if (devices) {
        for (i = 0; i < num_devices; i++) {
            device_destroy(lcd_class, devices[i].dev_num);
            cdev_del(&devices[i].cdev);
            mutex_destroy(&devices[i].log_mutex);
            mutex_destroy(&devices[i].text_mutex);
        }
        kfree(devices);
    }
    
    if (lcd_class) {
        class_destroy(lcd_class);
    }
    
    unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
    
    dbg_info(1, "LCD module unloaded successfully\n");
}

module_init(lcd_init);
module_exit(lcd_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DOMIoT");
MODULE_DESCRIPTION("LCD screen simulation module");
MODULE_VERSION("1.0.0"); 