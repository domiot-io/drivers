#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <phidget22.h>
#include <errno.h>

#define NUM_PORTS 6      // VINT Hub x6 has 6 ports (0-5)
#define MAX_DEVICES 10

typedef struct {
    int device_id;
    PhidgetDigitalInputHandle port_inputs[NUM_PORTS];  // One input per hub port
    int port_states[NUM_PORTS];                        // State of each port
    int port_opened[NUM_PORTS];                        // Whether each port opened successfully
    int port_has_activity[NUM_PORTS];                 // Whether port has shown state changes (indicating button connected)
} device_info_t;

static device_info_t devices[MAX_DEVICES];
static int num_devices = 0;
static volatile int running = 1;
static int verbose = 0;  // Global flag for verbose output

static void signal_handler(int sig) {
    running = 0;
}

static int write_sysfs_attribute(const char *path, const char *value) {
    FILE *file = fopen(path, "w");
    if (!file) {
        if (verbose) printf("ERROR: Failed to open sysfs file '%s' for writing: %s\n", path, strerror(errno));
        return -1;
    }
    
    int ret = fprintf(file, "%s", value);
    if (ret < 0) {
        if (verbose) printf("ERROR: Failed to write '%s' to sysfs file '%s': %s\n", value, path, strerror(errno));
        fclose(file);
        return -1;
    }
    
    if (fclose(file) != 0) {
        if (verbose) printf("ERROR: Failed to close sysfs file '%s': %s\n", path, strerror(errno));
        return -1;
    }
    
    if (verbose) printf("DEBUG: Successfully wrote '%s' to '%s'\n", value, path);
    return 0;
}

static void onDigitalInputStateChangeHandler(PhidgetDigitalInputHandle ch, void *ctx, int state) {
    device_info_t *device = (device_info_t *)ctx;
    
    // Find which port this handle corresponds to
    int port = -1;
    for (int i = 0; i < NUM_PORTS; i++) {
        if (device->port_inputs[i] == ch) {
            port = i;
            break;
        }
    }
    
    if (port >= 0 && port < NUM_PORTS && device->port_opened[port]) {
        // Mark this port as having activity (indicating a real button is connected)
        device->port_has_activity[port] = 1;
        
        // For floating/disconnected ports: raw=1 (pull-up) -> we want final=0
        // For low: raw=0 (pulled to ground) -> we want final=1  
        // For up: raw=1 (pull-up) -> we want final=0
        int inverted_state = (state == 0) ? 1 : 0;
        if (verbose) printf("State change detected: Port %d final=%d [raw=%d] %s (device %d)\n", 
               port, inverted_state, state, 
               state == 1 ? "(floating/released)" : "(pressed)", device->device_id);
        device->port_states[port] = inverted_state;
        
        // Update kernel module via sysfs
        char states_str[NUM_PORTS + 1];
        for (int i = 0; i < NUM_PORTS; i++) {
            // Only report state for ports that have shown activity, others stay 0
            char port_char = (device->port_has_activity[i] && device->port_states[i]) ? '1' : '0';
            states_str[i] = port_char;
            if (verbose) printf("DEBUG: Port %d: activity=%d state=%d -> char='%c'\n", 
                   i, device->port_has_activity[i], device->port_states[i], port_char);
        }
        states_str[NUM_PORTS] = '\0';
        
        if (verbose) printf("Updating sysfs with states: %s\n", states_str);
        
        char sysfs_path[256];
        snprintf(sysfs_path, sizeof(sysfs_path), 
                "/sys/class/phidgetvintx6/phidgetvintx6%d/input_states", device->device_id);
        
        if (write_sysfs_attribute(sysfs_path, states_str) < 0) {
            if (verbose) printf("Failed to update input states for device %d\n", device->device_id);
        } else {
            if (verbose) printf("Successfully updated input states for device %d\n", device->device_id);
        }
    } else {
        if (verbose) printf("Warning: State change from unknown or unopened port (device %d)\n", device->device_id);
    }
}

static int setup_device(device_info_t *device) {
    PhidgetReturnCode ret;
    const char *desc;
    
    // Initialize port_opened array
    for (int i = 0; i < NUM_PORTS; i++) {
        device->port_opened[i] = 0;
        device->port_has_activity[i] = 0;  // No activity detected yet
    }
    
    // set up digital inputs for each hub port (0-5)
    for (int port = 0; port < NUM_PORTS; port++) {
        if (verbose) printf("Setting up hub port %d...\n", port);
        
        ret = PhidgetDigitalInput_create(&device->port_inputs[port]);
        if (ret != EPHIDGET_OK) {
            Phidget_getErrorDescription(ret, &desc);
            if (verbose) printf("Failed to create digital input for port %d: (%d) %s\n", port, ret, desc);
            device->port_inputs[port] = NULL;
            continue;
        }
        
        // channel to 0 (first channel on the device connected to this port)
        ret = Phidget_setChannel((PhidgetHandle)device->port_inputs[port], 0);
        if (ret != EPHIDGET_OK) {
            Phidget_getErrorDescription(ret, &desc);
            if (verbose) printf("Failed to set channel 0 for port %d: (%d) %s\n", port, ret, desc);
            PhidgetDigitalInput_delete(&device->port_inputs[port]);
            device->port_inputs[port] = NULL;
            continue;
        }
        
        // Set the hub port (0-5)
        ret = Phidget_setHubPort((PhidgetHandle)device->port_inputs[port], port);
        if (ret != EPHIDGET_OK) {
            Phidget_getErrorDescription(ret, &desc);
            if (verbose) printf("Failed to set hub port %d: (%d) %s\n", port, ret, desc);
            PhidgetDigitalInput_delete(&device->port_inputs[port]);
            device->port_inputs[port] = NULL;
            continue;
        }
        
        // Set as hub port device
        ret = Phidget_setIsHubPortDevice((PhidgetHandle)device->port_inputs[port], 1);
        if (ret != EPHIDGET_OK) {
            Phidget_getErrorDescription(ret, &desc);
            if (verbose) printf("Failed to set hub port device for port %d: (%d) %s\n", port, ret, desc);
            PhidgetDigitalInput_delete(&device->port_inputs[port]);
            device->port_inputs[port] = NULL;
            continue;
        }
        
        // Set state change handler
        ret = PhidgetDigitalInput_setOnStateChangeHandler(device->port_inputs[port], 
                                                         onDigitalInputStateChangeHandler, device);
        if (ret != EPHIDGET_OK) {
            Phidget_getErrorDescription(ret, &desc);
            if (verbose) printf("Failed to set state change handler for port %d: (%d) %s\n", port, ret, desc);
            PhidgetDigitalInput_delete(&device->port_inputs[port]);
            device->port_inputs[port] = NULL;
            continue;
        }
        
        if (verbose) printf("Successfully set up digital input for hub port %d\n", port);
    }
    
    return 0;
}

static int open_device(device_info_t *device) {
    PhidgetReturnCode ret;
    const char *desc;
    int opened_ports = 0;
    
    // Open digital inputs for all hub ports - they should all be available
    for (int port = 0; port < NUM_PORTS; port++) {
        if (device->port_inputs[port] == NULL) {
            continue; // Skip ports that weren't created
        }
        
        if (verbose) printf("Attempting to open hub port %d...\n", port);
        ret = Phidget_openWaitForAttachment((PhidgetHandle)device->port_inputs[port], 2000);
        if (ret != EPHIDGET_OK) {
            Phidget_getErrorDescription(ret, &desc);
            if (verbose) printf("Could not open hub port %d: (%d) %s\n", port, ret, desc);
            
            // Clean up this port
            PhidgetDigitalInput_delete(&device->port_inputs[port]);
            device->port_inputs[port] = NULL;
            continue;
        }
        
        if (verbose) printf("Hub port %d opened successfully\n", port);
        opened_ports++;
        device->port_opened[port] = 1;  // Mark this port as successfully opened
        
        // Initialize all ports to 0 - only ports with activity will report their real state
        device->port_states[port] = 0;
        if (verbose) printf("Hub port %d initialized to state 0 (will update on first activity)\n", port);
    }
    
    if (verbose) printf("Device %d: Successfully opened %d/%d hub ports\n", device->device_id, opened_ports, NUM_PORTS);
    
    return opened_ports > 0 ? 0 : -1; // Success if at least one port is active
}

static void close_device(device_info_t *device) {
    // Close digital inputs for all ports
    for (int port = 0; port < NUM_PORTS; port++) {
        if (device->port_inputs[port]) {
            Phidget_close((PhidgetHandle)device->port_inputs[port]);
            PhidgetDigitalInput_delete(&device->port_inputs[port]);
            device->port_inputs[port] = NULL;
        }
        device->port_opened[port] = 0;  // Reset opened flag
        device->port_has_activity[port] = 0;  // Reset activity flag
    }
}

int main(int argc, char *argv[]) {
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else {
            printf("Usage: %s [-v|--verbose]\n", argv[0]);
            printf("  -v, --verbose    Enable verbose output\n");
            return 1;
        }
    }
    
    if (verbose) printf("Starting PhidgetVINTx6 daemon (Hub Port Mode)\n");
    if (verbose) printf("Scanning for devices on hub ports 0-5...\n");
    
    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize devices based on available sysfs entries
    for (int i = 0; i < MAX_DEVICES; i++) {
        char sysfs_path[256];
        snprintf(sysfs_path, sizeof(sysfs_path), 
                "/sys/class/phidgetvintx6/phidgetvintx6%d", i);
        
        if (access(sysfs_path, F_OK) == 0) {
            devices[num_devices].device_id = i;
            
            // Initialize states and opened flags
            memset(devices[num_devices].port_states, 0, sizeof(devices[num_devices].port_states));
            memset(devices[num_devices].port_opened, 0, sizeof(devices[num_devices].port_opened));
            memset(devices[num_devices].port_has_activity, 0, sizeof(devices[num_devices].port_has_activity));
            
            // Setup device to scan all hub ports
            if (setup_device(&devices[num_devices]) == 0) {
                if (open_device(&devices[num_devices]) == 0) {
                    if (verbose) printf("Device %d initialized successfully\n", i);
                    num_devices++;
                } else {
                    if (verbose) printf("Failed to open any ports for device %d\n", i);
                    close_device(&devices[num_devices]);
                }
            } else {
                if (verbose) printf("Failed to setup device %d\n", i);
            }
        }
    }
    
    if (num_devices == 0) {
        if (verbose) printf("No devices found. Exiting.\n");
        return 1;
    }
    
    if (verbose) printf("Managing %d devices\n", num_devices);
    
    // Set initial daemon status to connected for all devices
    for (int i = 0; i < num_devices; i++) {
        char sysfs_path[256];
        snprintf(sysfs_path, sizeof(sysfs_path), 
                "/sys/class/phidgetvintx6/phidgetvintx6%d/daemon_status", devices[i].device_id);
        write_sysfs_attribute(sysfs_path, "1");
        
        // Set initial input states - all start at 0, will update when buttons show activity
        char states_str[NUM_PORTS + 1];
        for (int j = 0; j < NUM_PORTS; j++) {
            // All ports start at 0 until they show activity
            states_str[j] = '0';
            if (verbose) printf("DEBUG: Port %d: activity=%d state=%d -> initial char='0'\n", 
                   j, devices[i].port_has_activity[j], devices[i].port_states[j]);
        }
        states_str[NUM_PORTS] = '\0';
        
        snprintf(sysfs_path, sizeof(sysfs_path), 
                "/sys/class/phidgetvintx6/phidgetvintx6%d/input_states", devices[i].device_id);
        write_sysfs_attribute(sysfs_path, states_str);
    }
    
    if (verbose) printf("Daemon ready. Listening for input changes on all hub ports...\n");
    
    // Main loop - keep it simple like main3.c to ensure proper event processing
    while (running) {
        // Sleep for 100ms - same as main3.c
        // This allows Phidget event callbacks to be processed properly
        usleep(100000);
    }
    
    if (verbose) printf("Shutting down daemon\n");
    
    // Cleanup
    for (int i = 0; i < num_devices; i++) {
        close_device(&devices[i]);
    }
    
    return 0;
} 