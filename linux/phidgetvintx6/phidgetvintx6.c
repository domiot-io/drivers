#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define DEVICE_NAME "phidgetvintx6"
#define CLASS_NAME "phidgetvintx6"
#define NUM_CHANNELS 6
#define BUFFER_SIZE (NUM_CHANNELS + 1)
#define MAX_READERS 10
#define MAX_DEVICES 10

// debug: 0=errors only, 1=+init/cleanup, 2=+operations, 3=+verbose
static int debug_level = 1;
module_param(debug_level, int, 0644);
MODULE_PARM_DESC(debug_level, "Debug level: 0=errors, 1=init/cleanup, 2=operations, 3=verbose (default: 1)");

static int num_devices = 1;
module_param(num_devices, int, 0644);
MODULE_PARM_DESC(num_devices, "Number of phidgetvintx6 devices to create (default: 1, max: 10)");

// debug macros to reduce overhead
#define dbg_err(fmt, ...) printk(KERN_ERR "phidgetvintx6: " fmt, ##__VA_ARGS__)
#define dbg_info(level, fmt, ...) do { if (debug_level >= level) printk(KERN_INFO "phidgetvintx6: " fmt, ##__VA_ARGS__); } while(0)
#define dbg_dev_info(level, dev_id, fmt, ...) do { if (debug_level >= level) printk(KERN_INFO "phidgetvintx6%d: " fmt, dev_id, ##__VA_ARGS__); } while(0)

struct phidgetvintx6_reader {
    struct list_head list;
    wait_queue_head_t wait;
    int state_changed;
    struct phidgetvintx6_device *device;
};

struct phidgetvintx6_device {
    int device_id;
    struct device *device;
    char channel_states[NUM_CHANNELS];
    char prev_channel_states[NUM_CHANNELS];
    char output_states[NUM_CHANNELS];
    struct list_head readers_list;
    spinlock_t readers_lock;
    struct mutex state_mutex;
    int daemon_connected;
};

static int major_number;
static struct class *phidgetvintx6_class = NULL;
static struct phidgetvintx6_device *devices = NULL;

struct phidgetvintx6_reader;

static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static unsigned int dev_poll(struct file *filep, struct poll_table_struct *wait);

// Sysfs attributes for communication with userspace daemon
static ssize_t input_states_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t input_states_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);
static ssize_t output_states_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t daemon_status_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t daemon_status_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);

static DEVICE_ATTR(input_states, 0664, input_states_show, input_states_store);
static DEVICE_ATTR(output_states, 0444, output_states_show, NULL);
static DEVICE_ATTR(daemon_status, 0664, daemon_status_show, daemon_status_store);

static struct attribute *phidgetvintx6_attrs[] = {
    &dev_attr_input_states.attr,
    &dev_attr_output_states.attr,
    &dev_attr_daemon_status.attr,
    NULL,
};

static const struct attribute_group phidgetvintx6_attr_group = {
    .attrs = phidgetvintx6_attrs,
};

static struct file_operations fops = {
    .open = dev_open,
    .read = dev_read,
    .write = dev_write,
    .release = dev_release,
    .poll = dev_poll,
};

// Sysfs attribute implementations
static ssize_t input_states_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct phidgetvintx6_device *phidget_dev = dev_get_drvdata(dev);
    if (!phidget_dev) return -ENODEV;
    
    mutex_lock(&phidget_dev->state_mutex);
    memcpy(buf, phidget_dev->channel_states, NUM_CHANNELS);
    buf[NUM_CHANNELS] = '\n';
    buf[NUM_CHANNELS + 1] = '\0';
    mutex_unlock(&phidget_dev->state_mutex);
    
    return NUM_CHANNELS + 1;
}

static ssize_t input_states_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct phidgetvintx6_device *phidget_dev = dev_get_drvdata(dev);
    struct phidgetvintx6_reader *reader;
    int i, changed = 0;
    
    if (!phidget_dev) return -ENODEV;
    if (count < NUM_CHANNELS) return -EINVAL;
    
    mutex_lock(&phidget_dev->state_mutex);
    
    // Save previous states
    memcpy(phidget_dev->prev_channel_states, phidget_dev->channel_states, NUM_CHANNELS);
    
    // Update channel states from daemon
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (buf[i] == '0' || buf[i] == '1') {
            phidget_dev->channel_states[i] = buf[i];
            if (phidget_dev->channel_states[i] != phidget_dev->prev_channel_states[i]) {
                changed = 1;
            }
        }
    }
    
    mutex_unlock(&phidget_dev->state_mutex);
    
    // Wake up waiting readers if state changed
    if (changed) {
        spin_lock(&phidget_dev->readers_lock);
        list_for_each_entry(reader, &phidget_dev->readers_list, list) {
            reader->state_changed = 1;
            wake_up_interruptible(&reader->wait);
        }
        spin_unlock(&phidget_dev->readers_lock);
        
        dbg_dev_info(3, phidget_dev->device_id, "Input states updated from daemon: %.6s\n", phidget_dev->channel_states);
    }
    
    return count;
}

static ssize_t output_states_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct phidgetvintx6_device *phidget_dev = dev_get_drvdata(dev);
    if (!phidget_dev) return -ENODEV;
    
    mutex_lock(&phidget_dev->state_mutex);
    memcpy(buf, phidget_dev->output_states, NUM_CHANNELS);
    buf[NUM_CHANNELS] = '\n';
    buf[NUM_CHANNELS + 1] = '\0';
    mutex_unlock(&phidget_dev->state_mutex);
    
    return NUM_CHANNELS + 1;
}

static ssize_t daemon_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct phidgetvintx6_device *phidget_dev = dev_get_drvdata(dev);
    if (!phidget_dev) return -ENODEV;
    
    return sprintf(buf, "%d\n", phidget_dev->daemon_connected);
}

static ssize_t daemon_status_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct phidgetvintx6_device *phidget_dev = dev_get_drvdata(dev);
    int status;
    
    if (!phidget_dev) return -ENODEV;
    
    if (kstrtoint(buf, 10, &status) == 0) {
        phidget_dev->daemon_connected = (status != 0);
        dbg_dev_info(2, phidget_dev->device_id, "Daemon status: %s\n", 
                     phidget_dev->daemon_connected ? "connected" : "disconnected");
    }
    
    return count;
}

static int __init phidgetvintx6_init(void) {
    int i, j;
    char device_name[32];
    int ret = 0;
    
    if (num_devices < 1 || num_devices > MAX_DEVICES) {
        dbg_err("Invalid num_devices (%d). Must be 1-%d\n", num_devices, MAX_DEVICES);
        return -EINVAL;
    }
    
    dbg_info(1, "Initializing %d phidgetvintx6 device(s)\n", num_devices);

    devices = kzalloc(num_devices * sizeof(struct phidgetvintx6_device), GFP_KERNEL);
    if (!devices) {
        dbg_err("Failed to allocate memory for devices\n");
        return -ENOMEM;
    }

    major_number = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_number < 0) {
        dbg_err("Failed to register a major number\n");
        kfree(devices);
        return major_number;
    }
    dbg_info(1, "Registered correctly with major number %d\n", major_number);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)
    phidgetvintx6_class = class_create(CLASS_NAME);
#else
    phidgetvintx6_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
    if (IS_ERR(phidgetvintx6_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        kfree(devices);
        dbg_err("Failed to register device class\n");
        return PTR_ERR(phidgetvintx6_class);
    }
    dbg_info(1, "Device class registered correctly\n");

    // create multiple devices
    for (i = 0; i < num_devices; i++) {
        devices[i].device_id = i;
        INIT_LIST_HEAD(&devices[i].readers_list);
        spin_lock_init(&devices[i].readers_lock);
        mutex_init(&devices[i].state_mutex);
        devices[i].daemon_connected = 0;
        
        // Initialize channel states to all zeros
        for (j = 0; j < NUM_CHANNELS; j++) {
            devices[i].channel_states[j] = '0';
            devices[i].prev_channel_states[j] = '0';
            devices[i].output_states[j] = '0';
        }
        
        snprintf(device_name, sizeof(device_name), "%s%d", DEVICE_NAME, i);
        
        devices[i].device = device_create(phidgetvintx6_class, NULL, MKDEV(major_number, i), NULL, device_name);
        if (IS_ERR(devices[i].device)) {
            dbg_err("Failed to create device %s\n", device_name);
            ret = PTR_ERR(devices[i].device);
            goto cleanup_devices;
        }
        
        // Set device driver data for sysfs attributes
        dev_set_drvdata(devices[i].device, &devices[i]);
        
        // Create sysfs attributes
        ret = sysfs_create_group(&devices[i].device->kobj, &phidgetvintx6_attr_group);
        if (ret) {
            dbg_err("Failed to create sysfs attributes for device %d\n", i);
            device_destroy(phidgetvintx6_class, MKDEV(major_number, i));
            goto cleanup_devices;
        }
        
        dbg_dev_info(1, i, "Device created correctly\n");
        dbg_dev_info(2, i, "Initial channel states: %.6s\n", devices[i].channel_states);
        dbg_info(1, "Sysfs interface: /sys/class/%s/%s%d/{input_states,output_states,daemon_status}\n", 
                 CLASS_NAME, DEVICE_NAME, i);
    }
           
    return 0;

cleanup_devices:
    for (j = 0; j < i; j++) {
        sysfs_remove_group(&devices[j].device->kobj, &phidgetvintx6_attr_group);
        device_destroy(phidgetvintx6_class, MKDEV(major_number, j));
    }
    class_destroy(phidgetvintx6_class);
    unregister_chrdev(major_number, DEVICE_NAME);
    kfree(devices);
    return ret;
}

static void __exit phidgetvintx6_exit(void) {
    struct phidgetvintx6_reader *reader, *tmp;
    int i;
    
    if (devices) {
        for (i = 0; i < num_devices; i++) {
            spin_lock(&devices[i].readers_lock);
            list_for_each_entry_safe(reader, tmp, &devices[i].readers_list, list) {
                list_del(&reader->list);
                kfree(reader);
            }
            spin_unlock(&devices[i].readers_lock);
            
            sysfs_remove_group(&devices[i].device->kobj, &phidgetvintx6_attr_group);
            device_destroy(phidgetvintx6_class, MKDEV(major_number, i));
        }
        kfree(devices);
    }
    
    class_unregister(phidgetvintx6_class);
    class_destroy(phidgetvintx6_class);
    unregister_chrdev(major_number, DEVICE_NAME);
    dbg_info(1, "Driver unloaded\n");
}

static int dev_open(struct inode *inodep, struct file *filep) {
    struct phidgetvintx6_reader *reader;
    int minor = iminor(inodep);
    
    if (minor >= num_devices) {
        dbg_err("Invalid minor number %d\n", minor);
        return -ENODEV;
    }
    
    reader = kmalloc(sizeof(struct phidgetvintx6_reader), GFP_KERNEL);
    if (!reader) {
        return -ENOMEM;
    }
    
    init_waitqueue_head(&reader->wait);
    reader->state_changed = 1;
    reader->device = &devices[minor];
    
    spin_lock(&devices[minor].readers_lock);
    list_add(&reader->list, &devices[minor].readers_list);
    spin_unlock(&devices[minor].readers_lock);
    
    filep->private_data = reader;
    dbg_dev_info(2, minor, "Device opened\n");
    return 0;
}

static int dev_release(struct inode *inodep, struct file *filep) {
    struct phidgetvintx6_reader *reader = filep->private_data;
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

static ssize_t dev_read(struct file *filep, char *buffer, size_t len, loff_t *offset) {
    struct phidgetvintx6_reader *reader = filep->private_data;
    char message[BUFFER_SIZE];
    int errors = 0;
    
    if (!reader || !reader->device) {
        dbg_err("Invalid reader or device pointer\n");
        return -EFAULT;
    }
    
    // Check if daemon is connected
    if (!reader->device->daemon_connected) {
        dbg_dev_info(2, reader->device->device_id, "Daemon not connected\n");
        return -ENODEV;
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
        dbg_dev_info(2, reader->device->device_id, "Failed to send %d characters to user\n", errors);
        return -EFAULT;
    }
    
    dbg_dev_info(3, reader->device->device_id, "Read channel states: %.6s\n", message);
    return BUFFER_SIZE;
}

static ssize_t dev_write(struct file *filep, const char *buffer, size_t len, loff_t *offset) {
    struct phidgetvintx6_reader *writer_reader = filep->private_data;
    struct phidgetvintx6_device *dev = writer_reader->device;
    char *user_input = NULL;
    int i, valid_digits = 0;
    
    if (!dev) {
        dbg_err("Invalid device pointer\n");
        return -EFAULT;
    }
    
    // Check if daemon is connected
    if (!dev->daemon_connected) {
        dbg_dev_info(2, dev->device_id, "Daemon not connected\n");
        return -ENODEV;
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
    
    // initialize all output channels to '0'
    memset(dev->output_states, '0', NUM_CHANNELS);
    
    // process input, only accepting '0' and '1'
    for (i = 0; i < len && valid_digits < NUM_CHANNELS; i++) {
        if (user_input[i] == '0' || user_input[i] == '1') {
            dev->output_states[valid_digits] = user_input[i];
            valid_digits++;
        } else if (user_input[i] == '\n' || user_input[i] == '\r') {
            continue;
        }
    }
    
    mutex_unlock(&dev->state_mutex);
    
    dbg_dev_info(2, dev->device_id, "Updated output states: %.6s (from %d valid digits)\n", 
                 dev->output_states, valid_digits);
    
    kfree(user_input);
    return len;
}

static unsigned int dev_poll(struct file *filep, struct poll_table_struct *wait) {
    struct phidgetvintx6_reader *reader = filep->private_data;
    unsigned int mask = 0;

    if (!reader || !reader->device) {
        return POLLERR;
    }

    poll_wait(filep, &reader->wait, wait);

    if (reader->state_changed) {
        mask |= POLLIN | POLLRDNORM;
    }

    // Always allow writing
    mask |= POLLOUT | POLLWRNORM;

    return mask;
}

module_init(phidgetvintx6_init);
module_exit(phidgetvintx6_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DOMIoT.org");
MODULE_DESCRIPTION("Phidget VINT Hub x6 IO Driver with Userspace Daemon Interface");
MODULE_VERSION("1.0"); 
