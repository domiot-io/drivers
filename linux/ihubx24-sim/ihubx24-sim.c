#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/random.h>
#include <linux/device.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/slab.h>

#define DEVICE_NAME "ihubx24-sim"
#define CLASS_NAME "ihubx24"
#define NUM_INPUTS 24
#define BUFFER_SIZE (NUM_INPUTS + 1)
#define MAX_READERS 10
#define MAX_DEVICES 10

// debug: 0=errors only, 1=+init/cleanup, 2=+operations, 3=+verbose
static int debug_level = 1;
module_param(debug_level, int, 0644);
MODULE_PARM_DESC(debug_level, "Debug level: 0=errors, 1=init/cleanup, 2=operations, 3=verbose (default: 1)");

static int num_devices = 1;
module_param(num_devices, int, 0644);
MODULE_PARM_DESC(num_devices, "Number of ihubx24-sim devices to create (default: 1, max: 10)");

// debug macros to reduce overhead
#define dbg_err(fmt, ...) printk(KERN_ERR "ihubx24-sim: " fmt, ##__VA_ARGS__)
#define dbg_info(level, fmt, ...) do { if (debug_level >= level) printk(KERN_INFO "ihubx24-sim: " fmt, ##__VA_ARGS__); } while(0)
#define dbg_dev_info(level, dev_id, fmt, ...) do { if (debug_level >= level) printk(KERN_INFO "ihubx24-sim%d: " fmt, dev_id, ##__VA_ARGS__); } while(0)

struct ihubx24_device {
    int device_id;
    struct device *device;
    struct timer_list input_timer;
    char input_states[NUM_INPUTS];
    char prev_input_states[NUM_INPUTS];
    struct list_head readers_list;
    spinlock_t readers_lock;
};

static int major_number;
static struct class *ihubx24_sim_class = NULL;
static struct ihubx24_device *devices = NULL;

struct ihubx24_sim_reader {
    struct list_head list;
    wait_queue_head_t wait;
    int state_changed;
    struct ihubx24_device *device;
};

static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static unsigned int dev_poll(struct file *filep, struct poll_table_struct *wait);

static struct file_operations fops = {
    .open = dev_open,
    .read = dev_read,
    .release = dev_release,
    .poll = dev_poll,
};

static void update_input_states(struct timer_list *t)
{
    struct ihubx24_device *dev = from_timer(dev, t, input_timer);
    int i;
    unsigned int random_val;
    int changed = 0;
    struct ihubx24_sim_reader *reader;
    
    // save previous states
    memcpy(dev->prev_input_states, dev->input_states, NUM_INPUTS);
    
    // random states for each input
    for (i = 0; i < NUM_INPUTS; i++) {
        get_random_bytes(&random_val, sizeof(random_val));
        dev->input_states[i] = (random_val % 2) ? '1' : '0';
        
        // Check if state changed
        if (dev->input_states[i] != dev->prev_input_states[i]) {
            changed = 1;
        }
    }
    
    // Reschedule the timer for 10 seconds later
    mod_timer(&dev->input_timer, jiffies + msecs_to_jiffies(10000));
    
    // Only log state updates if verbose debugging is enabled
    dbg_dev_info(3, dev->device_id, "Input states updated to %c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c\n",
           dev->input_states[0], dev->input_states[1], dev->input_states[2], dev->input_states[3],
           dev->input_states[4], dev->input_states[5], dev->input_states[6], dev->input_states[7],
           dev->input_states[8], dev->input_states[9], dev->input_states[10], dev->input_states[11],
           dev->input_states[12], dev->input_states[13], dev->input_states[14], dev->input_states[15],
           dev->input_states[16], dev->input_states[17], dev->input_states[18], dev->input_states[19],
           dev->input_states[20], dev->input_states[21], dev->input_states[22], dev->input_states[23]);
           
    // If state changed, wake up all waiting readers
    if (changed) {
        spin_lock(&dev->readers_lock);
        list_for_each_entry(reader, &dev->readers_list, list) {
            reader->state_changed = 1;
            wake_up_interruptible(&reader->wait);
        }
        spin_unlock(&dev->readers_lock);
    }
}

static int __init ihubx24_sim_init(void) {
    int i, j;
    unsigned int random_val;
    char device_name[32];
    int ret = 0;
    
    if (num_devices < 1 || num_devices > MAX_DEVICES) {
        dbg_err("Invalid num_devices (%d). Must be 1-%d\n", num_devices, MAX_DEVICES);
        return -EINVAL;
    }
    
    dbg_info(1, "Initializing %d ihubx24-sim device(s)\n", num_devices);

    devices = kzalloc(num_devices * sizeof(struct ihubx24_device), GFP_KERNEL);
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

    ihubx24_sim_class = class_create(CLASS_NAME);
    if (IS_ERR(ihubx24_sim_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        kfree(devices);
        dbg_err("Failed to register device class\n");
        return PTR_ERR(ihubx24_sim_class);
    }
    dbg_info(1, "Device class registered correctly\n");

    // create multiple devices
    for (i = 0; i < num_devices; i++) {
        devices[i].device_id = i;
        INIT_LIST_HEAD(&devices[i].readers_list);
        spin_lock_init(&devices[i].readers_lock);
        
        for (j = 0; j < NUM_INPUTS; j++) {
            get_random_bytes(&random_val, sizeof(random_val));
            devices[i].input_states[j] = (random_val % 2) ? '1' : '0';
            devices[i].prev_input_states[j] = devices[i].input_states[j];
        }
        
        snprintf(device_name, sizeof(device_name), "%s%d", DEVICE_NAME, i);
        
        devices[i].device = device_create(ihubx24_sim_class, NULL, MKDEV(major_number, i), NULL, device_name);
        if (IS_ERR(devices[i].device)) {
            dbg_err("Failed to create device %s\n", device_name);
            ret = PTR_ERR(devices[i].device);
            goto cleanup_devices;
        }
        
        // setup the timer for updating input states
        timer_setup(&devices[i].input_timer, update_input_states, 0);
        // Start the timer for the first time (10 seconds)
        mod_timer(&devices[i].input_timer, jiffies + msecs_to_jiffies(10000));
        
        dbg_dev_info(1, i, "Device created correctly\n");
        // Only show initial states if operations debugging is enabled
        dbg_dev_info(2, i, "Initial input states: %c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c\n",
               devices[i].input_states[0], devices[i].input_states[1], devices[i].input_states[2], devices[i].input_states[3],
               devices[i].input_states[4], devices[i].input_states[5], devices[i].input_states[6], devices[i].input_states[7],
               devices[i].input_states[8], devices[i].input_states[9], devices[i].input_states[10], devices[i].input_states[11],
               devices[i].input_states[12], devices[i].input_states[13], devices[i].input_states[14], devices[i].input_states[15],
               devices[i].input_states[16], devices[i].input_states[17], devices[i].input_states[18], devices[i].input_states[19],
               devices[i].input_states[20], devices[i].input_states[21], devices[i].input_states[22], devices[i].input_states[23]);
    }
           
    return 0;

cleanup_devices:
    for (j = 0; j < i; j++) {
        del_timer(&devices[j].input_timer);
        device_destroy(ihubx24_sim_class, MKDEV(major_number, j));
    }
    class_destroy(ihubx24_sim_class);
    unregister_chrdev(major_number, DEVICE_NAME);
    kfree(devices);
    return ret;
}

static void __exit ihubx24_sim_exit(void) {
    struct ihubx24_sim_reader *reader, *tmp;
    int i;
    
    if (devices) {
        for (i = 0; i < num_devices; i++) {
            del_timer(&devices[i].input_timer);
            
            spin_lock(&devices[i].readers_lock);
            list_for_each_entry_safe(reader, tmp, &devices[i].readers_list, list) {
                list_del(&reader->list);
                kfree(reader);
            }
            spin_unlock(&devices[i].readers_lock);
            
            device_destroy(ihubx24_sim_class, MKDEV(major_number, i));
        }
        kfree(devices);
    }
    
    class_unregister(ihubx24_sim_class);
    class_destroy(ihubx24_sim_class);
    unregister_chrdev(major_number, DEVICE_NAME);
    dbg_info(1, "Driver unloaded\n");
}

static int dev_open(struct inode *inodep, struct file *filep) {
    struct ihubx24_sim_reader *reader;
    int minor = iminor(inodep);
    
    if (minor >= num_devices) {
        dbg_err("Invalid minor number %d\n", minor);
        return -ENODEV;
    }
    
    reader = kmalloc(sizeof(struct ihubx24_sim_reader), GFP_KERNEL);
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
    struct ihubx24_sim_reader *reader = filep->private_data;
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
    int errors = 0;
    char message[BUFFER_SIZE];
    struct ihubx24_sim_reader *reader = filep->private_data;
    
    if (!reader || !reader->device) {
        return -EFAULT;
    }

    // Wait for state change if needed (for blocking reads)
    if (!reader->state_changed) {
        if (filep->f_flags & O_NONBLOCK) {
            return -EAGAIN;
        }
        if (wait_event_interruptible(reader->wait, reader->state_changed)) {
            return -ERESTARTSYS;
        }
    }
    
    reader->state_changed = 0;
    
    memcpy(message, reader->device->input_states, NUM_INPUTS);
    message[NUM_INPUTS] = '\n';  // Add newline
    
    errors = copy_to_user(buffer, message, BUFFER_SIZE);
    
    if (errors != 0) {
        dbg_dev_info(2, reader->device->device_id, "Failed to send %d characters to the user\n", errors);
        return -EFAULT;
    }
    
    // log successful reads if verbose debugging is enabled
    dbg_dev_info(3, reader->device->device_id, "Sent input states to user\n");
    return BUFFER_SIZE;
}

static unsigned int dev_poll(struct file *filep, struct poll_table_struct *wait) {
    unsigned int mask = 0;
    struct ihubx24_sim_reader *reader = filep->private_data;
    
    if (!reader || !reader->device) {
        return POLLERR;
    }
    
    poll_wait(filep, &reader->wait, wait);
    
    if (reader->state_changed) {
        mask |= POLLIN | POLLRDNORM;
    }
    
    return mask;
}

module_init(ihubx24_sim_init);
module_exit(ihubx24_sim_exit); 

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DOMIoT");
MODULE_DESCRIPTION("Input Hub x24 digital input channels module for simulation.");
MODULE_VERSION("1.0.0");
