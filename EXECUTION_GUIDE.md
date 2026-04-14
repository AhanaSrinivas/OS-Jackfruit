# OS Jackfruit - Execution Guide with Screenshots

## Prerequisites (Before Starting)

### On Ubuntu 22.04/24.04 VM
1. Secure Boot must be OFF
2. Terminal access (2 terminals will be needed)
3. `sudo` access

### Install Dependencies (Run in Terminal 1)
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

---

## Detailed Execution Steps with Screenshots

### PHASE 1: Build and Module Setup

#### Step 1.1: Navigate to Project Directory
```bash
cd /path/to/OS-Jackfruit
pwd
```
*(Verify you're in the correct directory)*

#### Step 1.2: Clean Previous Builds
```bash
make clean
ls -la
```
*(Show project files exist)*

**Screenshot 1A:** Terminal showing project directory contents (engine.c, monitor.c, Makefile, etc.)
![alt text](image.png)

#### Step 1.3: Build Project
```bash
make
```
*(Should compile without errors)*

**Screenshot 1B:** Terminal showing successful build output ending with "All targets built successfully" or similar
![alt text](image-1.png)


#### Step 1.4: Verify Binaries Created
```bash
ls -lh engine monitor.ko memory_hog cpu_hog io_pulse
file engine monitor.ko
file memory_hog cpu_hog io_pulse
```

**Screenshot 1C:** Terminal showing all binaries exist with correct types. The workload binaries should be static ELF executables so they can run inside Alpine.
![alt text](image-2.png)


#### Step 1.5: Load Kernel Module
```bash
sudo insmod monitor.ko
```

#### Step 1.6: Verify Module Loaded
```bash
lsmod | grep monitor
ls -l /dev/container_monitor
sudo dmesg | tail -5
```

**Screenshot 1D:** Terminal showing:
- `monitor` in lsmod output
- `/dev/container_monitor` device exists
- Kernel message "monitor: module loaded" or similar in dmesg
![alt text](image-3.png)


---

### PHASE 2: Prepare Root Filesystems

#### Step 2.1: Download Alpine Linux
```bash
mkdir -p rootfs-base
cd rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz
cd ..
ls -la rootfs-base/ | head -10
```

**Screenshot 2A:** Terminal showing rootfs-base directory with Alpine files (bin, etc, usr, etc.)



#### Step 2.2: Create Per-Container Copies
```bash
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
ls -ld rootfs-*
```

**Screenshot 2B:** Terminal showing three directories: rootfs-base, rootfs-alpha, rootfs-beta
![alt text](image-4.png)


#### Step 2.3: Copy Test Workloads into Containers
```bash
cp memory_hog rootfs-alpha/
cp cpu_hog rootfs-beta/
ls rootfs-alpha/ | grep memory_hog
ls rootfs-beta/ | grep cpu_hog
```

**Screenshot 2C:** Terminal confirming workload binaries are in each container's rootfs
![alt text](image-5.png)


---

### PHASE 3: Start Supervisor and Verify Socket

#### Step 3.1: Open Terminal 1 (Supervisor Terminal)
Keep this terminal dedicated to the supervisor. Do not close it.

#### Step 3.2: Start Supervisor (Pin to CPU Core 0)
```bash
sudo taskset -c 0 ./engine supervisor ./rootfs-base
```
*(Should output "Supervisor waiting for connections..." or similar; do NOT exit)*
Run this only once. If you restart the supervisor, run start commands again in the new session.

**Screenshot 3A:** Terminal 1 showing supervisor startup message
![alt text](image-7.png)


#### Step 3.3: Verify Socket Created (Open Terminal 2)
Open a **second terminal** and keep Terminal 1 running.

```bash
cd /path/to/OS-Jackfruit
ls -l /tmp/mini_runtime.sock
```

**Screenshot 3B:** Terminal 2 showing `/tmp/mini_runtime.sock` exists after the supervisor is running
![alt text](image-6.png)


---

### PHASE 4: Launch Containers via CLI

**All commands in this phase run in Terminal 2 (the CLI terminal). Terminal 1 must keep running the supervisor.**

#### Step 4.1: Start Container Alpha (Memory Workload, High Priority)
```bash
sudo ./engine start alpha ./rootfs-alpha /memory_hog --soft-mib 48 --hard-mib 80
echo "Alpha started"
sleep 2
```

**Screenshot 4A:** Terminal 2 showing "start" command and "OK: started..." response
![alt text](image-9.png)


#### Step 4.2: Start Container Beta (CPU Workload, Low Priority)
```bash
sudo ./engine start beta ./rootfs-beta "/cpu_hog 60" --nice 19
echo "Beta started"
sleep 2
```

**Screenshot 4B:** Terminal 2 showing "start" command and "OK: started..." response
![alt text](image-10.png)


#### Step 4.3: List Containers (PS Command)
```bash
sudo ./engine ps
```

**Screenshot 4C (IMPORTANT - Screenshot #2 for submission):** Terminal output of ps showing:
- Container IDs (alpha, beta)
- PIDs
- States (RUNNING)
- Memory limits (soft/hard MiB)
- Log file paths
(This demonstrates metadata tracking)
![alt text](image-11.png)


#### Step 4.4: Check Container Logs - Alpha
```bash
sudo ./engine ps
sudo ./engine logs alpha
sudo tail -n 20 logs/alpha.log
```

**Screenshot 4D (IMPORTANT - Screenshot #3 for submission):** This is the bounded-buffer logging screenshot. Capture terminal output where `./engine ps` shows alpha in `STATE=running` and logs show `allocation=...` lines.

If alpha is already `STATE=exited`, run this once and retry Step 4.4:
```bash
sudo ./engine start alpha ./rootfs-alpha /memory_hog --soft-mib 48 --hard-mib 80
sleep 3
```
![alt text](image-15.png)


#### Step 4.5: Check Container Logs - Beta
```bash
sudo ./engine ps
sudo ./engine start beta ./rootfs-beta "/cpu_hog 60" --nice 19
sleep 2
sudo ./engine ps
sudo ./engine logs beta
sudo tail -n 20 logs/beta.log
sleep 1
```

If beta exits too quickly, empty beta logs are acceptable. Do not spend time debugging this step; continue directly to Step 4.6 and Phase 5.

#### Step 4.6: Monitor Kernel Messages (Open Terminal 3)
Open a **third terminal** for kernel monitoring:

```bash
cd /path/to/OS-Jackfruit
sudo dmesg -w
```
*(This shows live kernel messages; keep this running)*

**Screenshot 4E:** Terminal 3 showing dmesg output
![alt text](image-16.png)
![alt text](image-17.png)


---

### PHASE 5: Memory Limit Demonstration

#### Step 5.1: Let Containers Run (30 seconds in Terminal 2)
```bash
# If alpha is already killed from an earlier run, restart it first:
# sudo ./engine start alpha ./rootfs-alpha /memory_hog --soft-mib 48 --hard-mib 80

# Wait 30 seconds to let alpha's memory grow
sleep 30
```
During this time, observe Terminal 3 (dmesg) for soft/hard limit messages.

#### Step 5.2: Check Soft Limit Warning in Kernel Logs
```bash
# In Terminal 2:
sudo dmesg | grep -i "SOFT LIMIT" | tail -10
```

**Screenshot 5A (IMPORTANT - Screenshot #5 for submission):** dmesg output showing:
`[container_monitor] SOFT LIMIT container=... pid=... rss=... limit=...`
![alt text](image-18.png)


#### Step 5.3: Force Memory Pressure (Let Alpha Allocate More)
```bash
# Let alpha continue running; observe if it hits hard limit
sleep 60
sudo dmesg | grep -i "HARD LIMIT" | tail -10
```

**Screenshot 5B (IMPORTANT - Screenshot #6 for submission):** dmesg output showing:
`[container_monitor] HARD LIMIT container=... pid=... rss=... limit=...`
![alt text](image-19.png)

#### Step 5.4: Check PS Output After Kill
```bash
sudo ./engine ps
```

**Screenshot 5C:** Terminal showing alpha state changed to KILLED_HARD_LIMIT
![alt text](image-20.png)
![alt text](image-21.png)


---

### PHASE 6: Scheduling Experiment (CPU Allocation)

#### Step 6.1: Measure CPU Time Distribution
```bash
# Terminal 2 - Let both containers run for 10 seconds
sleep 10
```

#### Step 6.2: Check CPU Usage with Top
Open Terminal 4:
```bash
top -b -n 1 | grep -E "cpu_hog|memory_hog|%CPU|USER"
```

**Screenshot 6A (IMPORTANT - Screenshot #7 for submission):** top output showing:
- alpha (cpu_hog) with ~98-99% CPU
- beta (cpu_hog) with ~0-1% CPU
(Demonstrates CFS scheduling with nice values)

#### Step 6.3: Show Both Still Running
```bash
sudo ./engine ps
```

**Screenshot 6B:** ps output showing both containers in RUNNING state but with different CPU allocation
![alt text](image-22.png)

---

### PHASE 7: Graceful Shutdown and Cleanup

#### Step 7.1: Stop Container Alpha
```bash
# Terminal 2
sudo ./engine stop alpha
sleep 2
```

**Screenshot 7A:** Terminal output showing stop command accepted
![alt text](image-23.png)

#### Step 7.2: Stop Container Beta
```bash
sudo ./engine stop beta
sleep 2
```

#### Step 7.3: Verify Both Stopped
```bash
sudo ./engine ps
```

**Screenshot 7B:** ps output showing alpha and beta states = STOPPED

#### Step 7.4: Stop Supervisor (Press Ctrl+C in Terminal 1)
```bash
# In Terminal 1 (supervisor terminal), press Ctrl+C
# Expected: "Supervisor shutting down..."
```

**Screenshot 7C (IMPORTANT - Screenshot #8 for submission):** Terminal 1 showing:
- Supervisor shutdown messages
- "All children reaped" or similar
- Clean exit

#### Step 7.5: Verify No Zombies
```bash
# Terminal 2
ps aux | grep -E "engine|cpu_hog|memory_hog|defunct" | grep -v grep
# Should return only grep itself, no defunct processes
```

**Screenshot 7D:** Terminal showing no zombie processes (only grep in the output)

#### Step 7.6: Unload Kernel Module
```bash
sudo rmmod monitor
lsmod | grep monitor
# Should return nothing
```

**Screenshot 7E:** Terminal confirming monitor module removed
![alt text](image-24.png)

#### Step 7.7: Check Final Logs
```bash
ls -la logs/
cat logs/alpha.log | tail -20
cat logs/beta.log | tail -20
```

**Screenshot 7F:** Terminal showing per-container log files with captured output
![alt text](image-25.png)
![alt text](image-26.png)

#### Step 7.8: Final Kernel Log Check
```bash
sudo dmesg | tail -20
```

**Screenshot 7G:** Terminal showing final dmesg entries including module unload message
![alt text](image-27.png)

---

## Summary: 8 Required Screenshots for Submission

| Screenshot # | When to Take | What Should Show | File Name |
|---|---|---|---|
| 1 | After module load (Step 1.6) | Monitor module loaded, /dev/container_monitor exists, dmesg confirmation | `1_module_loaded.png` |
| 2 | After `./engine ps` (Step 4.3) | Both alpha and beta RUNNING, metadata (PIDs, limits, log paths) | `2_metadata_tracking.png` |
| 3 | After `./engine logs alpha` (Step 4.4) | Log file contents showing captured output from memory_hog | `3_bounded_buffer_logging.png` |
| 4 | Show CLI command and response (Step 4.1 or 4.2) | A start command being issued and supervisor responding "OK: started" | `4_cli_ipc.png` |
| 5 | After soft limit detection (Step 5.2) | dmesg output containing `[container_monitor] SOFT LIMIT ...` | `5_soft_limit_warning.png` |
| 6 | After hard limit kill (Step 5.3) | dmesg output containing `[container_monitor] HARD LIMIT ...` + ps output showing KILLED/HARD-LIMIT-style state | `6_hard_limit_enforcement.png` |
| 7 | After top command (Step 6.2) | top output showing alpha ~99% CPU, beta ~0% CPU (starved) | `7_scheduling_experiment.png` |
| 8 | After supervisor shutdown (Step 7.4) | Supervisor exit messages, confirmed all children reaped, no zombies (Step 7.5) | `8_clean_teardown.png` |

---

## Quick Command Cheat Sheet (Copy-Paste Ready)

### Terminal 1 (Supervisor):
```bash
cd /path/to/OS-Jackfruit
sudo taskset -c 0 ./engine supervisor ./rootfs-base
# Press Ctrl+C when done
```

### Terminal 2 (CLI Commands):
```bash
cd /path/to/OS-Jackfruit

# Build and setup
make
sudo insmod monitor.ko

# Filesystem prep
mkdir -p rootfs-base && cd rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz
cd ..
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
cp memory_hog rootfs-alpha/
cp cpu_hog rootfs-beta/

# Containers
sudo ./engine start alpha ./rootfs-alpha /memory_hog --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta "/cpu_hog 60" --nice 19
sudo ./engine ps
sudo ./engine logs alpha
sleep 60

# Cleanup
sudo ./engine stop alpha
sudo ./engine stop beta
sudo ./engine ps
sudo rmmod monitor
```

---

## Troubleshooting

**Q: "Permission denied" when running insmod?**
A: Use `sudo`: `sudo insmod monitor.ko`

**Q: Supervisor won't start?**
A: Check `/tmp/mini_runtime.sock` doesn't exist from previous run: `rm -f /tmp/mini_runtime.sock`

**Q: CLI command hangs?**
A: Make sure Terminal 1 with supervisor is still running and responsive

**Q: No dmesg output for limits?**
A: Limits only appear when container actually allocates memory (memory_hog does this)

**Q: Can't create alpine rootfs?**
A: Check internet connection; alternatively, use any small Linux image you have available

