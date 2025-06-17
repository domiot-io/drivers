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
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/list.h>

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

struct iohubx24_reader {
    struct list_head list;
    wait_queue_head_t wait;
    int state_changed;
    struct iohubx24_device *device;
};

struct iohubx24_device {
    dev_t dev_num;
    struct cdev cdev;
    struct device *device;
    int minor;
    char channel_states[NUM_CHANNELS];
    char prev_channel_states[NUM_CHANNELS];
    struct mutex state_mutex;
    struct list_head readers_list;
    spinlock_t readers_lock;
};

static int major_number;
static struct class *iohubx24_class = NULL;
static struct iohubx24_device *devices = NULL;

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);
static unsigned int device_poll(struct file *, struct poll_table_struct *);

static struct file_operations fops = {
    .open = device_open,
    .read = device_read,
    .write = device_write,
    .release = device_release,
    .poll = device_poll,
};

static int device_open(struct inode *inodep, struct file *filep)
{
    struct iohubx24_reader *reader;
    int minor = iminor(inodep);
    
    if (minor >= num_devices) {
        dbg_err("Invalid minor number %d\n", minor);
        return -ENODEV;
    }
    
    reader = kmalloc(sizeof(struct iohubx24_reader), GFP_KERNEL);
    if (!reader) {
        return -ENOMEM;
    }
    
    init_waitqueue_head(&reader->wait);
    reader->state_changed = 1; // First read should always succeed
    reader->device = &devices[minor];
    
    spin_lock(&devices[minor].readers_lock);
    list_add(&reader->list, &devices[minor].readers_list);
    spin_unlock(&devices[minor].readers_lock);
    
    filep->private_data = reader;
    dbg_dev_info(2, minor, "Device opened\n");
    return 0;
}

static int device_release(struct inode *inodep, struct file *filep)
{
    struct iohubx24_reader *reader = filep->private_data;
    int minor = iminor(inodep);
    
    if (reader && reader->device) {
        spin_lock(&reader->device->readers_lock);
        list_del(&reader->list);
        spin_unlock(&reader->device->readers_lock);
        kfree(reader);
    }
    
    dbg_dev_info(2, minor, "Device closed\n");
    return 0;
}

static ssize_t device_read(struct file *filep, char *buffer, size_t len, loff_t *offset)
{
    struct iohubx24_reader *reader = filep->private_data;
    char message[BUFFER_SIZE];
    int errors = 0;
    
    if (!reader || !reader->device) {
        dbg_err("Invalid reader or device pointer\n");
        return -EFAULT;
    }
    
    // wait for state change if needed (for blocking reads)
    if (!reader->state_changed) {
        if (filep->f_flags & O_NONBLOCK) {
            return -EAGAIN;
        }
        if (wait_event_interruptible(reader->wait, reader->state_changed)) {
            return -ERESTARTSYS;
        }
    }
    
    reader->state_changed = 0;
    
    mutex_lock(&reader->device->state_mutex);
    
    // copy channel states and add newline
    memcpy(message, reader->device->channel_states, NUM_CHANNELS);
    message[NUM_CHANNELS] = '\n';
    
    mutex_unlock(&reader->device->state_mutex);
    
    errors = copy_to_user(buffer, message, BUFFER_SIZE);
    if (errors != 0) {
        dbg_dev_info(2, reader->device->minor, "Failed to send %d characters to user\n", errors);
        return -EFAULT;
    }
    
    dbg_dev_info(3, reader->device->minor, "Read channel states: %.24s\n", message);
    return BUFFER_SIZE;
}

static ssize_t device_write(struct file *filep, const char *buffer, size_t len, loff_t *offset)
{
    struct iohubx24_reader *writer_reader = filep->private_data;
    struct iohubx24_device *dev = writer_reader->device;
    char *user_input = NULL;
    int i, valid_digits = 0;
    int changed = 0;
    struct iohubx24_reader *reader;
    
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
    
    // save previous states
    memcpy(dev->prev_channel_states, dev->channel_states, NUM_CHANNELS);
    
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
    
    // Check if state changed
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (dev->channel_states[i] != dev->prev_channel_states[i]) {
            changed = 1;
            break;
        }
    }
    
    mutex_unlock(&dev->state_mutex);
    
    // If state changed, wake up all waiting readers
    if (changed) {
        spin_lock(&dev->readers_lock);
        list_for_each_entry(reader, &dev->readers_list, list) {
            reader->state_changed = 1;
            wake_up_interruptible(&reader->wait);
        }
        spin_unlock(&dev->readers_lock);
    }
    
    dbg_dev_info(2, dev->minor, "Updated channel states: %.24s (from %d valid digits)\n", 
                 dev->channel_states, valid_digits);
    
    kfree(user_input);
    return len;
}

static unsigned int device_poll(struct file *filep, struct poll_table_struct *wait)
{
    struct iohubx24_reader *reader = filep->private_data;
    unsigned int mask = 0;

    if (!reader || !reader->device) {
        return POLLERR;
    }

    poll_wait(filep, &reader->wait, wait);

    if (reader->state_changed) {
        mask |= POLLIN | POLLRDNORM;
    }

    return mask;
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
        INIT_LIST_HEAD(&devices[i].readers_list);
        spin_lock_init(&devices[i].readers_lock);
        
        memset(devices[i].channel_states, '0', NUM_CHANNELS);
        memset(devices[i].prev_channel_states, '0', NUM_CHANNELS);
        
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
    struct iohubx24_reader *reader, *tmp;
    int i;
    
    dbg_info(1, "Unloading module\n");
    
    if (devices) {
        for (i = 0; i < num_devices; i++) {
            // Clean up any remaining readers
            spin_lock(&devices[i].readers_lock);
            list_for_each_entry_safe(reader, tmp, &devices[i].readers_list, list) {
                list_del(&reader->list);
                kfree(reader);
            }
            spin_unlock(&devices[i].readers_lock);
            
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