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

#define DEVICE_NAME "ohubx24-sim"
#define CLASS_NAME "ohubx24"
#define MAX_DEVICES 10
#define MAX_LOG_ENTRIES 30
#define OUTPUT_LENGTH 24
#define LOG_ENTRY_SIZE 64

// compatibility macros
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)
    #define CLASS_CREATE_COMPAT(name) class_create(name)
#else
    #define CLASS_CREATE_COMPAT(name) class_create(THIS_MODULE, name)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
    #define HAVE_PROC_OPS
#endif

static int num_devices = 1;
module_param(num_devices, int, S_IRUGO);
MODULE_PARM_DESC(num_devices, "Number of devices to create (default: 1)");

struct ohubx24_device {
    dev_t dev_num;
    struct cdev cdev;
    struct device *device;
    int minor;
    char log_entries[MAX_LOG_ENTRIES][LOG_ENTRY_SIZE];
    int log_count;
    int log_head;
    struct mutex log_mutex;
};

static int major_number;
static struct class *ohubx24_class = NULL;
static struct ohubx24_device *devices = NULL;

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);
static void add_log_entry(struct ohubx24_device *dev, const char *output);
static void write_log_to_file(struct ohubx24_device *dev);

static struct file_operations fops = {
    .open = device_open,
    .write = device_write,
    .release = device_release,
};

static int device_open(struct inode *inodep, struct file *filep)
{
    int minor = iminor(inodep);
    if (minor >= num_devices) {
        return -ENODEV;
    }
    
    filep->private_data = &devices[minor];
    printk(KERN_INFO "ohubx24-sim: Device %d has been opened\n", minor);
    return 0;
}

static ssize_t device_write(struct file *filep, const char *buffer, size_t len, loff_t *offset)
{
    struct ohubx24_device *dev = (struct ohubx24_device *)filep->private_data;
    char *user_input = NULL;
    char output[OUTPUT_LENGTH + 1];
    int i, valid_digits = 0;
    
    if (len == 0) {
        return 0;
    }
    
    // allocate memory for user input
    user_input = kmalloc(len + 1, GFP_KERNEL);
    if (!user_input) {
        return -ENOMEM;
    }
    
    if (copy_from_user(user_input, buffer, len)) {
        kfree(user_input);
        return -EFAULT;
    }
    user_input[len] = '\0';
    
    // initialize output with zeros
    memset(output, '0', OUTPUT_LENGTH);
    output[OUTPUT_LENGTH] = '\0';
    
    for (i = 0; i < len && valid_digits < OUTPUT_LENGTH; i++) {
        if (user_input[i] == '0' || user_input[i] == '1') {
            output[valid_digits] = user_input[i];
            valid_digits++;
        } else if (user_input[i] == '\n' || user_input[i] == '\r') {
            // Ignore newlines and carriage returns
            continue;
        }
    }
    
    // add log entry and write to file
    add_log_entry(dev, output);
    write_log_to_file(dev);
    
    printk(KERN_INFO "ohubx24-sim: Device %d received: %s\n", dev->minor, output);
    
    kfree(user_input);
    return len;
}

static int device_release(struct inode *inodep, struct file *filep)
{
    struct ohubx24_device *dev = (struct ohubx24_device *)filep->private_data;
    printk(KERN_INFO "ohubx24-sim: Device %d has been closed\n", dev->minor);
    return 0;
}

static void add_log_entry(struct ohubx24_device *dev, const char *output)
{
    struct timespec64 ts;
    struct tm tm;
    
    mutex_lock(&dev->log_mutex);
    
    ktime_get_real_ts64(&ts);
    time64_to_tm(ts.tv_sec, 0, &tm);
    
    // Format: "YYYY-MM-DD HH:MM:SS <24-digit-output>"
    snprintf(dev->log_entries[dev->log_head], LOG_ENTRY_SIZE,
             "%04ld-%02d-%02d %02d:%02d:%02d %s",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, output);
    
    dev->log_head = (dev->log_head + 1) % MAX_LOG_ENTRIES;
    if (dev->log_count < MAX_LOG_ENTRIES) {
        dev->log_count++;
    }
    
    mutex_unlock(&dev->log_mutex);
}

static void write_log_to_file(struct ohubx24_device *dev)
{
    struct file *file;
    char filename[32];
    int i, idx;
    loff_t pos = 0;
    
    snprintf(filename, sizeof(filename), "/tmp/ohubx24-output%d", dev->minor);
    
    file = filp_open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (IS_ERR(file)) {
        printk(KERN_ERR "ohubx24-sim: Failed to open log file %s\n", filename);
        return;
    }
    
    mutex_lock(&dev->log_mutex);
    
    // Write entries in reverse order (newest first)
    for (i = 0; i < dev->log_count; i++) {
        idx = (dev->log_head - 1 - i + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
        kernel_write(file, dev->log_entries[idx], strlen(dev->log_entries[idx]), &pos);
        kernel_write(file, "\n", 1, &pos);
    }
    
    mutex_unlock(&dev->log_mutex);
    
    filp_close(file, NULL);
}

static int __init ohubx24_init(void)
{
    int i, result;
    
    printk(KERN_INFO "ohubx24-sim: Initializing with %d devices\n", num_devices);
    
    if (num_devices <= 0 || num_devices > MAX_DEVICES) {
        printk(KERN_ERR "ohubx24-sim: Invalid number of devices: %d\n", num_devices);
        return -EINVAL;
    }
    
    // allocate major number
    result = alloc_chrdev_region(&major_number, 0, num_devices, DEVICE_NAME);
    if (result < 0) {
        printk(KERN_ERR "ohubx24-sim: Failed to allocate major number\n");
        return result;
    }
    major_number = MAJOR(major_number);
    
    ohubx24_class = CLASS_CREATE_COMPAT(CLASS_NAME);
    if (IS_ERR(ohubx24_class)) {
        unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
        printk(KERN_ERR "ohubx24-sim: Failed to create device class\n");
        return PTR_ERR(ohubx24_class);
    }
    
    devices = kcalloc(num_devices, sizeof(struct ohubx24_device), GFP_KERNEL);
    if (!devices) {
        class_destroy(ohubx24_class);
        unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
        return -ENOMEM;
    }
    
    // initialize devices
    for (i = 0; i < num_devices; i++) {
        devices[i].dev_num = MKDEV(major_number, i);
        devices[i].minor = i;
        devices[i].log_count = 0;
        devices[i].log_head = 0;
        mutex_init(&devices[i].log_mutex);
        
        // initialize cdev
        cdev_init(&devices[i].cdev, &fops);
        devices[i].cdev.owner = THIS_MODULE;
        
        result = cdev_add(&devices[i].cdev, devices[i].dev_num, 1);
        if (result) {
            printk(KERN_ERR "ohubx24-sim: Failed to add cdev for device %d\n", i);
            goto cleanup_devices;
        }
        
        // create device
        devices[i].device = device_create(ohubx24_class, NULL, devices[i].dev_num, NULL, DEVICE_NAME "%d", i);
        if (IS_ERR(devices[i].device)) {
            printk(KERN_ERR "ohubx24-sim: Failed to create device %d\n", i);
            cdev_del(&devices[i].cdev);
            result = PTR_ERR(devices[i].device);
            goto cleanup_devices;
        }
        
        printk(KERN_INFO "ohubx24-sim: Created /dev/%s%d\n", DEVICE_NAME, i);
    }
    
    printk(KERN_INFO "ohubx24-sim: Module loaded successfully\n");
    return 0;
    
cleanup_devices:
    for (i--; i >= 0; i--) {
        device_destroy(ohubx24_class, devices[i].dev_num);
        cdev_del(&devices[i].cdev);
        mutex_destroy(&devices[i].log_mutex);
    }
    kfree(devices);
    class_destroy(ohubx24_class);
    unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
    return result;
}

static void __exit ohubx24_exit(void)
{
    int i;
    
    printk(KERN_INFO "ohubx24-sim: Unloading module\n");
    
    if (devices) {
        for (i = 0; i < num_devices; i++) {
            device_destroy(ohubx24_class, devices[i].dev_num);
            cdev_del(&devices[i].cdev);
            mutex_destroy(&devices[i].log_mutex);
        }
        kfree(devices);
    }
    
    if (ohubx24_class) {
        class_destroy(ohubx24_class);
    }
    
    unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
    
    printk(KERN_INFO "ohubx24-sim: Module unloaded successfully\n");
}

module_init(ohubx24_init);
module_exit(ohubx24_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DOMIoT");
MODULE_DESCRIPTION("Output Hub x24 digital output channels module for simulation");
MODULE_VERSION("1.0.0");