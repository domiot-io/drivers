#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/version.h>

#define DEVICE_NAME "iohubx24-sim"
#define CLASS_NAME "iohubx24"
#define NUM_CHANNELS 24
#define BUFFER_SIZE (NUM_CHANNELS + 1)
#define MAX_DEVICES 10

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
module_param(num_devices, int, 0644);
MODULE_PARM_DESC(num_devices, "Number of iohubx24-sim devices to create (default: 1, max: 10)");

// debug macros to reduce overhead
#define dbg_err(fmt, ...) printk(KERN_ERR "iohubx24-sim: " fmt, ##__VA_ARGS__)
#define dbg_info(level, fmt, ...) do { if (debug_level >= level) printk(KERN_INFO "iohubx24-sim: " fmt, ##__VA_ARGS__); } while(0)
#define dbg_dev_info(level, dev_id, fmt, ...) do { if (debug_level >= level) printk(KERN_INFO "iohubx24-sim%d: " fmt, dev_id, ##__VA_ARGS__); } while(0)

struct iohubx24_device {
    dev_t dev_num;
    struct cdev cdev;
    struct device *device;
    int minor;
    char channel_states[NUM_CHANNELS];
    struct mutex state_mutex;
};

static int major_number;
static struct class *iohubx24_class = NULL;
static struct iohubx24_device *devices = NULL;

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);

static struct file_operations fops = {
    .open = device_open,
    .read = device_read,
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
    dbg_dev_info(2, minor, "Device opened\n");
    return 0;
}

static int device_release(struct inode *inodep, struct file *filep)
{
    int minor = iminor(inodep);
    dbg_dev_info(2, minor, "Device closed\n");
    return 0;
}

static ssize_t device_read(struct file *filep, char *buffer, size_t len, loff_t *offset)
{
    struct iohubx24_device *dev = (struct iohubx24_device *)filep->private_data;
    char message[BUFFER_SIZE];
    int errors = 0;
    
    if (!dev) {
        dbg_err("Invalid device pointer\n");
        return -EFAULT;
    }
    
    // Prevent multiple simultaneous reads
    if (*offset > 0) {
        return 0; // EOF
    }
    
    mutex_lock(&dev->state_mutex);
    
    // copy channel states and add newline
    memcpy(message, dev->channel_states, NUM_CHANNELS);
    message[NUM_CHANNELS] = '\n';
    
    mutex_unlock(&dev->state_mutex);
    
    errors = copy_to_user(buffer, message, BUFFER_SIZE);
    if (errors != 0) {
        dbg_dev_info(2, dev->minor, "Failed to send %d characters to user\n", errors);
        return -EFAULT;
    }
    
    *offset = BUFFER_SIZE;
    dbg_dev_info(3, dev->minor, "Read channel states: %.24s\n", message);
    return BUFFER_SIZE;
}

static ssize_t device_write(struct file *filep, const char *buffer, size_t len, loff_t *offset)
{
    struct iohubx24_device *dev = (struct iohubx24_device *)filep->private_data;
    char *user_input = NULL;
    int i, valid_digits = 0;
    
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
    
    mutex_lock(&dev->state_mutex);
    
    // initialize all channels to '0'
    memset(dev->channel_states, '0', NUM_CHANNELS);
    
    // input, only accepting '0' and '1'
    for (i = 0; i < len && valid_digits < NUM_CHANNELS; i++) {
        if (user_input[i] == '0' || user_input[i] == '1') {
            dev->channel_states[valid_digits] = user_input[i];
            valid_digits++;
        } else if (user_input[i] == '\n' || user_input[i] == '\r') {
            continue;
        }
    }
    
    mutex_unlock(&dev->state_mutex);
    
    dbg_dev_info(2, dev->minor, "Updated channel states: %.24s (from %d valid digits)\n", 
                 dev->channel_states, valid_digits);
    
    kfree(user_input);
    return len;
}

static int __init iohubx24_init(void)
{
    int i, result;
    
    if (num_devices <= 0 || num_devices > MAX_DEVICES) {
        dbg_err("Invalid num_devices (%d). Must be 1-%d\n", num_devices, MAX_DEVICES);
        return -EINVAL;
    }
    
    dbg_info(1, "Initializing %d iohubx24-sim device(s)\n", num_devices);
    
    result = alloc_chrdev_region(&major_number, 0, num_devices, DEVICE_NAME);
    if (result < 0) {
        dbg_err("Failed to allocate major number\n");
        return result;
    }
    major_number = MAJOR(major_number);
    dbg_info(1, "Registered correctly with major number %d\n", major_number);
    
    iohubx24_class = CLASS_CREATE_COMPAT(CLASS_NAME);
    if (IS_ERR(iohubx24_class)) {
        unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
        dbg_err("Failed to create device class\n");
        return PTR_ERR(iohubx24_class);
    }
    dbg_info(1, "Device class created correctly\n");
    
    devices = kcalloc(num_devices, sizeof(struct iohubx24_device), GFP_KERNEL);
    if (!devices) {
        class_destroy(iohubx24_class);
        unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
        dbg_err("Failed to allocate memory for devices\n");
        return -ENOMEM;
    }
    
    for (i = 0; i < num_devices; i++) {
        devices[i].dev_num = MKDEV(major_number, i);
        devices[i].minor = i;
        mutex_init(&devices[i].state_mutex);
        
        memset(devices[i].channel_states, '0', NUM_CHANNELS);
        
        cdev_init(&devices[i].cdev, &fops);
        devices[i].cdev.owner = THIS_MODULE;
        
        result = cdev_add(&devices[i].cdev, devices[i].dev_num, 1);
        if (result) {
            dbg_err("Failed to add cdev for device %d\n", i);
            goto cleanup_devices;
        }
        
        devices[i].device = device_create(iohubx24_class, NULL, devices[i].dev_num, NULL, DEVICE_NAME "%d", i);
        if (IS_ERR(devices[i].device)) {
            dbg_err("Failed to create device %d\n", i);
            cdev_del(&devices[i].cdev);
            result = PTR_ERR(devices[i].device);
            goto cleanup_devices;
        }
        
        dbg_dev_info(1, i, "Device created correctly\n");
        dbg_dev_info(2, i, "Initial channel states: %.24s\n", devices[i].channel_states);
    }
    
    dbg_info(1, "Module loaded successfully\n");
    return 0;
    
cleanup_devices:
    for (i--; i >= 0; i--) {
        device_destroy(iohubx24_class, devices[i].dev_num);
        cdev_del(&devices[i].cdev);
        mutex_destroy(&devices[i].state_mutex);
    }
    kfree(devices);
    class_destroy(iohubx24_class);
    unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
    return result;
}

static void __exit iohubx24_exit(void)
{
    int i;
    
    dbg_info(1, "Unloading module\n");
    
    if (devices) {
        for (i = 0; i < num_devices; i++) {
            device_destroy(iohubx24_class, devices[i].dev_num);
            cdev_del(&devices[i].cdev);
            mutex_destroy(&devices[i].state_mutex);
        }
        kfree(devices);
    }
    
    if (iohubx24_class) {
        class_destroy(iohubx24_class);
    }
    
    unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
    
    dbg_info(1, "Module unloaded successfully\n");
}

module_init(iohubx24_init);
module_exit(iohubx24_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DOMIoT");
MODULE_DESCRIPTION("I/O Hub x24 digital channels module for simulation");
MODULE_VERSION("1.0.0"); 