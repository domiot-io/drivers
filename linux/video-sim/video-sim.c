#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/kstrtox.h>

#define DEVICE_NAME "video-sim"
#define CLASS_NAME "video"
#define MAX_DEVICES 10
#define VIDEO_MAX_CHARS 1024
#define PLAY_DURATION_SECONDS 20
#define MAX_PATH_LENGTH 1000

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
MODULE_PARM_DESC(num_devices, "Number of video devices to create (default: 1, max: 10)");

// debug macros to reduce overhead
#define dbg_err(fmt, ...) printk(KERN_ERR "video-sim: " fmt, ##__VA_ARGS__)
#define dbg_info(level, fmt, ...) do { if (debug_level >= level) printk(KERN_INFO "video-sim: " fmt, ##__VA_ARGS__); } while(0)
#define dbg_dev_info(level, dev_id, fmt, ...) do { if (debug_level >= level) printk(KERN_INFO "video-sim%d: " fmt, dev_id, ##__VA_ARGS__); } while(0)

enum video_state {
    VIDEO_STOPPED,
    VIDEO_PLAYING,
    VIDEO_PAUSED
};

struct video_sim_reader {
    struct list_head list;
    wait_queue_head_t wait;
    int data_available;
    struct video_device *device;
};

struct video_device {
    dev_t dev_num;
    struct cdev cdev;
    struct device *device;
    int minor;
    char current_text[VIDEO_MAX_CHARS + 1];
    struct mutex text_mutex;
    struct timer_list play_timer;
    struct timer_list time_update_timer;
    struct list_head readers_list;
    spinlock_t readers_lock;
    enum video_state state;
    struct mutex state_mutex;
    char video_src[MAX_PATH_LENGTH];
    int src_loaded;
    unsigned long pause_time;
    unsigned long play_start_time;
    unsigned long remaining_time_ms;
    unsigned long current_position_ms;
    int video_ended;
    int loop_enabled;
};

static int major_number;
static struct class *video_class = NULL;
static struct video_device *devices = NULL;

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);
static ssize_t device_read(struct file *, char *, size_t, loff_t *);
static unsigned int device_poll(struct file *, struct poll_table_struct *);

static struct file_operations fops = {
    .open = device_open,
    .read = device_read,
    .write = device_write,
    .release = device_release,
    .poll = device_poll,
};

static void read_timer_callback(struct timer_list *t)
{
    struct video_device *dev = from_timer(dev, t, play_timer);
    struct video_sim_reader *reader;
    
    mutex_lock(&dev->state_mutex);
    
    if (dev->state == VIDEO_PLAYING) {
        dbg_dev_info(2, dev->minor, "Play timer expired - video %s\n", 
                     dev->loop_enabled ? "restarting (loop enabled)" : "ended");
        
        if (dev->loop_enabled) {
            // Restart the video from the beginning
            dev->current_position_ms = 0;
            dev->remaining_time_ms = PLAY_DURATION_SECONDS * 1000;
            dev->play_start_time = jiffies;
            
            // Restart both timers
            mod_timer(&dev->play_timer, jiffies + msecs_to_jiffies(dev->remaining_time_ms));
            mod_timer(&dev->time_update_timer, jiffies + msecs_to_jiffies(100));
            
            // Wake up readers for the restart (position reset to 0.0)
            spin_lock(&dev->readers_lock);
            list_for_each_entry(reader, &dev->readers_list, list) {
                reader->data_available = 1;
                wake_up_interruptible(&reader->wait);
            }
            spin_unlock(&dev->readers_lock);
            
            dbg_dev_info(2, dev->minor, "Video restarted due to loop - notified readers\n");
        } else {
            // Normal end behavior
            dev->state = VIDEO_STOPPED;
            dev->remaining_time_ms = PLAY_DURATION_SECONDS * 1000; // Reset for next play
            dev->current_position_ms = PLAY_DURATION_SECONDS * 1000; // Set to end position
            dev->video_ended = 1;
            
            // Stop the time update timer
            del_timer(&dev->time_update_timer);
            
            // Wake up all waiting readers
            spin_lock(&dev->readers_lock);
            list_for_each_entry(reader, &dev->readers_list, list) {
                reader->data_available = 1;
                wake_up_interruptible(&reader->wait);
            }
            spin_unlock(&dev->readers_lock);
            
            dbg_dev_info(2, dev->minor, "Notified all readers that video ended\n");
        }
    }
    
    mutex_unlock(&dev->state_mutex);
}

static void time_update_callback(struct timer_list *t)
{
    struct video_device *dev = from_timer(dev, t, time_update_timer);
    struct video_sim_reader *reader;
    
    mutex_lock(&dev->state_mutex);
    
    if (dev->state == VIDEO_PLAYING) {
        // Wake up all waiting readers for the current time
        spin_lock(&dev->readers_lock);
        list_for_each_entry(reader, &dev->readers_list, list) {
            reader->data_available = 1;
            wake_up_interruptible(&reader->wait);
        }
        spin_unlock(&dev->readers_lock);
        
        // Schedule next update in 100ms if still playing and not at end
        if (dev->current_position_ms < PLAY_DURATION_SECONDS * 1000) {
            // Update current position (increment by 100ms for next time)
            dev->current_position_ms += 100;
            
            // Make sure we don't exceed the total duration
            if (dev->current_position_ms > PLAY_DURATION_SECONDS * 1000) {
                dev->current_position_ms = PLAY_DURATION_SECONDS * 1000;
            }
            
            mod_timer(&dev->time_update_timer, jiffies + msecs_to_jiffies(100));
        }
    }
    
    mutex_unlock(&dev->state_mutex);
}

static int device_open(struct inode *inodep, struct file *filep)
{
    int minor = iminor(inodep);
    struct video_sim_reader *reader;
    
    if (minor >= num_devices) {
        dbg_err("Invalid minor number %d\n", minor);
        return -ENODEV;
    }
    
    // For reading, create a reader structure
    if (filep->f_mode & FMODE_READ) {
        reader = kmalloc(sizeof(struct video_sim_reader), GFP_KERNEL);
        if (!reader) {
            return -ENOMEM;
        }
        
        init_waitqueue_head(&reader->wait);
        reader->data_available = 0; // No data available initially
        reader->device = &devices[minor];
        
        // Reset video ended flag and position when a new reader opens
        mutex_lock(&devices[minor].state_mutex);
        devices[minor].video_ended = 0;
        if (devices[minor].state == VIDEO_STOPPED) {
            devices[minor].current_position_ms = 0;
        }
        mutex_unlock(&devices[minor].state_mutex);
        
        spin_lock(&devices[minor].readers_lock);
        list_add(&reader->list, &devices[minor].readers_list);
        spin_unlock(&devices[minor].readers_lock);
        
        filep->private_data = reader;
        dbg_dev_info(2, minor, "Video device opened for reading\n");
    } else {
        filep->private_data = &devices[minor];
        dbg_dev_info(2, minor, "Video device opened for writing\n");
    }
    
    return 0;
}

static ssize_t device_write(struct file *filep, const char *buffer, size_t len, loff_t *offset)
{
    struct video_device *dev;
    char *user_input = NULL;
    char processed_text[VIDEO_MAX_CHARS + 1];
    int i, processed_len = 0;
    
    // Handle both reader and direct device access
    if (filep->f_mode & FMODE_READ) {
        struct video_sim_reader *reader = (struct video_sim_reader *)filep->private_data;
        dev = reader->device;
    } else {
        dev = (struct video_device *)filep->private_data;
    }
    
    if (!dev) {
        dbg_err("Invalid device pointer\n");
        return -EFAULT;
    }
    
    if (len == 0) {
        return 0;
    }
    
    // Limit to first 1024 characters as requested
    size_t actual_len = len > VIDEO_MAX_CHARS ? VIDEO_MAX_CHARS : len;
    
    user_input = kmalloc(actual_len + 1, GFP_KERNEL);
    if (!user_input) {
        dbg_err("Failed to allocate memory for user input\n");
        return -ENOMEM;
    }
    
    if (copy_from_user(user_input, buffer, actual_len)) {
        kfree(user_input);
        return -EFAULT;
    }
    user_input[actual_len] = '\0';
    
    // Process input text: take first 1024 characters, convert newlines to spaces
    memset(processed_text, 0, sizeof(processed_text));
    for (i = 0; i < actual_len && processed_len < VIDEO_MAX_CHARS; i++) {
        if (user_input[i] == '\n' || user_input[i] == '\r') {
            processed_text[processed_len] = ' ';
            processed_len++;
        } else if (user_input[i] >= 32 && user_input[i] <= 126) {
            processed_text[processed_len] = user_input[i];
            processed_len++;
        }
    }
    processed_text[processed_len] = '\0';
    
    // Remove trailing spaces and null terminate properly
    while (processed_len > 0 && processed_text[processed_len-1] == ' ') {
        processed_text[processed_len-1] = '\0';
        processed_len--;
    }
    
    mutex_lock(&dev->text_mutex);
    strncpy(dev->current_text, processed_text, VIDEO_MAX_CHARS);
    dev->current_text[VIDEO_MAX_CHARS] = '\0';
    mutex_unlock(&dev->text_mutex);
    
    // Process commands
    mutex_lock(&dev->state_mutex);
    
    if (strcmp(processed_text, "PAUSE") == 0) {
        if (dev->state == VIDEO_PLAYING) {
            // Calculate elapsed time and remaining time
            unsigned long current_time = jiffies;
            unsigned long elapsed_ms = jiffies_to_msecs(current_time - dev->play_start_time);
            
            del_timer(&dev->play_timer);
            del_timer(&dev->time_update_timer);
            dev->state = VIDEO_PAUSED;
            dev->pause_time = current_time;
            
            if (elapsed_ms < dev->remaining_time_ms) {
                dev->remaining_time_ms -= elapsed_ms;
            } else {
                dev->remaining_time_ms = 0;
            }
            
            dbg_dev_info(2, dev->minor, "PAUSE command accepted - video paused with %lu ms remaining\n", dev->remaining_time_ms);
        } else {
            dbg_dev_info(2, dev->minor, "PAUSE command ignored - video not currently playing\n");
        }
    } else if (strcmp(processed_text, "PLAY") == 0) {
        if (!dev->src_loaded) {
            dbg_dev_info(2, dev->minor, "PLAY command ignored - no video source loaded (need SET SRC and LOAD first)\n");
        } else if (dev->state == VIDEO_PLAYING) {
            dbg_dev_info(2, dev->minor, "PLAY command ignored - video already playing\n");
        } else {
            // Start or resume video
            dev->state = VIDEO_PLAYING;
            dev->play_start_time = jiffies;
            dev->video_ended = 0;
            
            if (dev->remaining_time_ms < PLAY_DURATION_SECONDS * 1000) {
                // Resume from paused position or continue partial playback
                mod_timer(&dev->play_timer, jiffies + msecs_to_jiffies(dev->remaining_time_ms));
                dbg_dev_info(2, dev->minor, "PLAY command accepted - resuming video with %lu ms remaining: %s (loop: %s)\n", 
                             dev->remaining_time_ms, dev->video_src, dev->loop_enabled ? "enabled" : "disabled");
            } else {
                // Start from beginning
                dev->current_position_ms = 0;
                mod_timer(&dev->play_timer, jiffies + msecs_to_jiffies(dev->remaining_time_ms));
                dbg_dev_info(2, dev->minor, "PLAY command accepted - starting %d second timer for video: %s (loop: %s)\n", 
                             PLAY_DURATION_SECONDS, dev->video_src, dev->loop_enabled ? "enabled" : "disabled");
            }
            
            // Start the time update timer immediately (first update in 0ms, then every 100ms)
            mod_timer(&dev->time_update_timer, jiffies);
        }
    } else if (strcmp(processed_text, "LOAD") == 0) {
        if (strlen(dev->video_src) > 0) {
            int was_playing = (dev->state == VIDEO_PLAYING);
            int was_paused = (dev->state == VIDEO_PAUSED);
            
            dev->src_loaded = 1;
            dev->remaining_time_ms = PLAY_DURATION_SECONDS * 1000; // Reset timer
            dev->current_position_ms = 0; // Reset position
            dev->video_ended = 0;
            
            if (was_playing || was_paused) {
                // Stop current playback and reset
                del_timer(&dev->play_timer);
                del_timer(&dev->time_update_timer);
                dev->state = VIDEO_STOPPED;
                dbg_dev_info(2, dev->minor, "LOAD command accepted - video reloaded and timer reset: %s (loop: %s)\n", 
                             dev->video_src, dev->loop_enabled ? "enabled" : "disabled");
            } else {
                dbg_dev_info(2, dev->minor, "LOAD command accepted - video loaded: %s (loop: %s)\n", 
                             dev->video_src, dev->loop_enabled ? "enabled" : "disabled");
            }
        } else {
            dbg_dev_info(2, dev->minor, "LOAD command ignored - no video source set (need SET SRC first)\n");
        }
    } else if (strncmp(processed_text, "SET LOOP=", 9) == 0) {
        const char *loop_value = processed_text + 9;
        
        if (strcasecmp(loop_value, "TRUE") == 0 || strcmp(loop_value, "1") == 0) {
            dev->loop_enabled = 1;
            dbg_dev_info(2, dev->minor, "SET LOOP command accepted - loop enabled\n");
        } else if (strcasecmp(loop_value, "FALSE") == 0 || strcmp(loop_value, "0") == 0) {
            dev->loop_enabled = 0;
            dbg_dev_info(2, dev->minor, "SET LOOP command accepted - loop disabled\n");
        } else {
            dbg_dev_info(2, dev->minor, "SET LOOP command ignored - invalid value '%s' (use TRUE or FALSE)\n", loop_value);
        }
    } else if (strncmp(processed_text, "SET SRC=", 8) == 0) {
        // Stop any current playback first
        if (dev->state == VIDEO_PLAYING || dev->state == VIDEO_PAUSED) {
            del_timer(&dev->play_timer);
            del_timer(&dev->time_update_timer);
            dev->state = VIDEO_STOPPED;
            dbg_dev_info(2, dev->minor, "Stopped current playback due to new SET SRC command\n");
        }
        
        // Reset loaded state and timer since we're setting a new (or same) source
        // Note: loop_enabled is NOT reset here - it persists across src changes
        dev->src_loaded = 0;
        dev->remaining_time_ms = PLAY_DURATION_SECONDS * 1000;
        dev->current_position_ms = 0;
        dev->video_ended = 0;
        
        // Set the new video source (extract path after "SET SRC=")
        const char *src_path = processed_text + 8;
        int src_len = strlen(src_path);
        if (src_len > 0 && src_len < MAX_PATH_LENGTH) {
            strncpy(dev->video_src, src_path, MAX_PATH_LENGTH - 1);
            dev->video_src[MAX_PATH_LENGTH - 1] = '\0';
            dbg_dev_info(2, dev->minor, "SET SRC command accepted - video source set to: %s (must call LOAD before PLAY) (loop: %s)\n", 
                         dev->video_src, dev->loop_enabled ? "enabled" : "disabled");
        } else if (src_len == 0) {
            memset(dev->video_src, 0, MAX_PATH_LENGTH);
            dbg_dev_info(2, dev->minor, "SET SRC command accepted - video source cleared (loop: %s)\n", 
                         dev->loop_enabled ? "enabled" : "disabled");
        } else {
            dbg_dev_info(2, dev->minor, "SET SRC command ignored - path too long (%d chars, max %d)\n", src_len, MAX_PATH_LENGTH - 1);
        }
    } else if (strncmp(processed_text, "SET CURRENT_TIME=", 17) == 0) {
        // Extract time value after "SET CURRENT_TIME="
        const char *time_str = processed_text + 17;
        char *dot_pos = strchr(time_str, '.');
        unsigned long seconds_part = 0;
        unsigned long ms_part = 0;
        unsigned long new_position_ms = 0;
        int valid_time = 0;
        
        if (dot_pos) {
            // Parse seconds.milliseconds format
            char seconds_str[16] = {0};
            char ms_str[8] = {0};
            int seconds_len = dot_pos - time_str;
            int ms_len = strlen(dot_pos + 1);
            
            if (seconds_len < 16 && ms_len <= 3) {
                strncpy(seconds_str, time_str, seconds_len);
                strncpy(ms_str, dot_pos + 1, ms_len);
                
                // Pad milliseconds to 3 digits
                if (ms_len == 1) {
                    strcat(ms_str, "00");  // .1 -> 100ms
                } else if (ms_len == 2) {
                    strcat(ms_str, "0");   // .12 -> 120ms
                }
                // .123 -> 123ms (already 3 digits)
                
                if (kstrtoul(seconds_str, 10, &seconds_part) == 0 && 
                    kstrtoul(ms_str, 10, &ms_part) == 0) {
                    new_position_ms = seconds_part * 1000 + ms_part;
                    valid_time = 1;
                }
            }
        } else {
            // Parse integer seconds format
            if (kstrtoul(time_str, 10, &seconds_part) == 0) {
                new_position_ms = seconds_part * 1000;
                valid_time = 1;
            }
        }
        
        if (valid_time && new_position_ms <= PLAY_DURATION_SECONDS * 1000) {
            unsigned long total_duration_ms = PLAY_DURATION_SECONDS * 1000;
            
            // Calculate remaining time from the new position
            dev->remaining_time_ms = total_duration_ms - new_position_ms;
            dev->current_position_ms = new_position_ms;
            
            // If video is currently playing, restart timer with new remaining time
            if (dev->state == VIDEO_PLAYING) {
                del_timer(&dev->play_timer);
                del_timer(&dev->time_update_timer);
                dev->play_start_time = jiffies;
                mod_timer(&dev->play_timer, jiffies + msecs_to_jiffies(dev->remaining_time_ms));
                mod_timer(&dev->time_update_timer, jiffies + msecs_to_jiffies(100));
                dbg_dev_info(2, dev->minor, "SET CURRENT_TIME command accepted - position set to %lu ms, %lu ms remaining (timer restarted) (loop: %s)\n", 
                             new_position_ms, dev->remaining_time_ms, dev->loop_enabled ? "enabled" : "disabled");
            } else {
                dbg_dev_info(2, dev->minor, "SET CURRENT_TIME command accepted - position set to %lu ms, %lu ms remaining (loop: %s)\n", 
                             new_position_ms, dev->remaining_time_ms, dev->loop_enabled ? "enabled" : "disabled");
            }
        } else {
            dbg_dev_info(2, dev->minor, "SET CURRENT_TIME command ignored - invalid time format or value > %d seconds\n", 
                         PLAY_DURATION_SECONDS);
        }
    } else {
        dbg_dev_info(2, dev->minor, "Video updated with text: \"%s\" (%d chars, accepted %zu of %zu)\n", 
                     processed_text, processed_len, actual_len, len);
    }
    
    mutex_unlock(&dev->state_mutex);
    
    kfree(user_input);
    return len; // Return original length to indicate all was "processed"
}

static ssize_t device_read(struct file *filep, char *buffer, size_t len, loff_t *offset)
{
    struct video_sim_reader *reader = (struct video_sim_reader *)filep->private_data;
    char time_message[32];
    size_t message_len;
    
    if (!reader || !reader->device) {
        dbg_err("Invalid reader or device pointer\n");
        return -EFAULT;
    }
    
    // Non-blocking mode
    if (filep->f_flags & O_NONBLOCK) {
        if (!reader->data_available) {
            return -EAGAIN;
        }
    } else {
        // Blocking mode - wait for data
        if (wait_event_interruptible(reader->wait, reader->data_available)) {
            return -ERESTARTSYS;
        }
    }
    
    // Reset data available flag
    reader->data_available = 0;
    
    mutex_lock(&reader->device->state_mutex);
    
    // Check if video has ended (only if loop is disabled)
    if (reader->device->video_ended && !reader->device->loop_enabled) {
        mutex_unlock(&reader->device->state_mutex);
        
        const char *end_message = "END\n";
        message_len = strlen(end_message);
        
        if (len < message_len) {
            return -EINVAL;
        }
        
        if (copy_to_user(buffer, end_message, message_len)) {
            return -EFAULT;
        }
        
        dbg_dev_info(2, reader->device->minor, "Read returned: END\n");
        return message_len;
    }
    
    // Format current time message
    unsigned long position_ms = reader->device->current_position_ms;
    unsigned long seconds = position_ms / 1000;
    unsigned long tenths = (position_ms % 1000) / 100;
    
    snprintf(time_message, sizeof(time_message), "CURRENT_TIME=%lu.%lu\n", seconds, tenths);
    message_len = strlen(time_message);
    
    mutex_unlock(&reader->device->state_mutex);
    
    if (len < message_len) {
        return -EINVAL;
    }
    
    if (copy_to_user(buffer, time_message, message_len)) {
        return -EFAULT;
    }
    
    dbg_dev_info(3, reader->device->minor, "Read returned: %s", time_message);
    
    return message_len;
}

static unsigned int device_poll(struct file *filep, struct poll_table_struct *wait)
{
    struct video_sim_reader *reader = (struct video_sim_reader *)filep->private_data;
    unsigned int mask = 0;
    
    if (!reader) {
        return POLLERR;
    }
    
    poll_wait(filep, &reader->wait, wait);
    
    if (reader->data_available) {
        mask |= POLLIN | POLLRDNORM;
    }
    
    // Always allow writing
    mask |= POLLOUT | POLLWRNORM;
    
    return mask;
}

static int device_release(struct inode *inodep, struct file *filep)
{
    if (filep->f_mode & FMODE_READ) {
        struct video_sim_reader *reader = (struct video_sim_reader *)filep->private_data;
        if (reader && reader->device) {
            spin_lock(&reader->device->readers_lock);
            list_del(&reader->list);
            spin_unlock(&reader->device->readers_lock);
            
            dbg_dev_info(2, reader->device->minor, "Video device closed (reader)\n");
            kfree(reader);
        }
    } else {
        struct video_device *dev = (struct video_device *)filep->private_data;
        if (dev) {
            dbg_dev_info(2, dev->minor, "Video device closed (writer)\n");
        }
    }
    return 0;
}

static int __init video_init(void)
{
    int i, result;
    
    if (num_devices <= 0 || num_devices > MAX_DEVICES) {
        dbg_err("Invalid number of devices: %d (must be 1-%d)\n", num_devices, MAX_DEVICES);
        return -EINVAL;
    }
    
    dbg_info(1, "Initializing %d video device(s)\n", num_devices);
    
    result = alloc_chrdev_region(&major_number, 0, num_devices, DEVICE_NAME);
    if (result < 0) {
        dbg_err("Failed to allocate major number\n");
        return result;
    }
    major_number = MAJOR(major_number);
    dbg_info(1, "Registered correctly with major number %d\n", major_number);
    
    video_class = CLASS_CREATE_COMPAT(CLASS_NAME);
    if (IS_ERR(video_class)) {
        unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
        dbg_err("Failed to create device class\n");
        return PTR_ERR(video_class);
    }
    dbg_info(1, "Device class created correctly\n");
    
    devices = kcalloc(num_devices, sizeof(struct video_device), GFP_KERNEL);
    if (!devices) {
        class_destroy(video_class);
        unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
        dbg_err("Failed to allocate memory for devices\n");
        return -ENOMEM;
    }
    
    for (i = 0; i < num_devices; i++) {
        devices[i].dev_num = MKDEV(major_number, i);
        devices[i].minor = i;
        mutex_init(&devices[i].text_mutex);
        mutex_init(&devices[i].state_mutex);
        INIT_LIST_HEAD(&devices[i].readers_list);
        spin_lock_init(&devices[i].readers_lock);
        devices[i].state = VIDEO_STOPPED;
        devices[i].src_loaded = 0;
        devices[i].remaining_time_ms = PLAY_DURATION_SECONDS * 1000;
        devices[i].current_position_ms = 0;
        devices[i].video_ended = 0;
        devices[i].pause_time = 0;
        devices[i].play_start_time = 0;
        devices[i].loop_enabled = 0; // Default: loop disabled
        memset(devices[i].video_src, 0, MAX_PATH_LENGTH);
        
        // Initialize video with empty text
        memset(devices[i].current_text, 0, sizeof(devices[i].current_text));
        
        cdev_init(&devices[i].cdev, &fops);
        result = cdev_add(&devices[i].cdev, devices[i].dev_num, 1);
        if (result) {
            dbg_err("Failed to add device %d\n", i);
            goto cleanup_devices;
        }
        
        devices[i].device = device_create(video_class, NULL, devices[i].dev_num, NULL, DEVICE_NAME "%d", i);
        if (IS_ERR(devices[i].device)) {
            cdev_del(&devices[i].cdev);
            dbg_err("Failed to create device %d\n", i);
            result = PTR_ERR(devices[i].device);
            goto cleanup_devices;
        }
        
        // Setup the timers but don't start them yet
        timer_setup(&devices[i].play_timer, read_timer_callback, 0);
        timer_setup(&devices[i].time_update_timer, time_update_callback, 0);
        
        dbg_info(1, "Video device %d created: /dev/" DEVICE_NAME "%d (loop: disabled)\n", i, i);
    }
    
    dbg_info(1, "Video-sim driver loaded successfully\n");
    return 0;

cleanup_devices:
    for (i = i - 1; i >= 0; i--) {
        del_timer(&devices[i].play_timer);
        del_timer(&devices[i].time_update_timer);
        device_destroy(video_class, devices[i].dev_num);
        cdev_del(&devices[i].cdev);
    }
    kfree(devices);
    class_destroy(video_class);
    unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
    return result;
}

static void __exit video_exit(void)
{
    struct video_sim_reader *reader, *tmp;
    int i;
    
    dbg_info(1, "Unloading video-sim driver\n");
    
    if (devices) {
        for (i = 0; i < num_devices; i++) {
            del_timer(&devices[i].play_timer);
            del_timer(&devices[i].time_update_timer);
            
            // Clean up any remaining readers
            spin_lock(&devices[i].readers_lock);
            list_for_each_entry_safe(reader, tmp, &devices[i].readers_list, list) {
                list_del(&reader->list);
                kfree(reader);
            }
            spin_unlock(&devices[i].readers_lock);
            
            device_destroy(video_class, devices[i].dev_num);
            cdev_del(&devices[i].cdev);
        }
        kfree(devices);
    }
    
    if (video_class) {
        class_destroy(video_class);
    }
    
    unregister_chrdev_region(MKDEV(major_number, 0), num_devices);
    
    dbg_info(1, "Video-sim driver unloaded\n");
}

module_init(video_init);
module_exit(video_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DOMIoT");
MODULE_DESCRIPTION("A video simulation driver.");
MODULE_VERSION("1.0"); 