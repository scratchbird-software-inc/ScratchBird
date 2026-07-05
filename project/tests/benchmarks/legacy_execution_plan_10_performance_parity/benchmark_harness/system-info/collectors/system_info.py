#!/usr/bin/env python3
"""
System Information Collector for Benchmark Results

Gathers comprehensive hardware and software information to correlate
with benchmark performance. Works across Linux, macOS, and Windows.
"""

import json
import os
import platform as platform_module
import re
import shutil
import socket
import subprocess
from dataclasses import dataclass, asdict, field
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


@dataclass
class CPUInfo:
    """CPU information."""
    vendor: str = "Unknown"
    model: str = "Unknown"
    architecture: str = "Unknown"
    physical_cores: int = 0
    logical_cores: int = 0
    threads_per_core: int = 0
    base_frequency_mhz: float = 0.0
    max_frequency_mhz: float = 0.0
    cache_l1_kb: int = 0
    cache_l2_kb: int = 0
    cache_l3_kb: int = 0
    flags: List[str] = field(default_factory=list)
    virtualization: str = "Unknown"


@dataclass
class GPUInfo:
    """GPU information."""
    vendor: str = "Unknown"
    model: str = "Unknown"
    vram_mb: int = 0
    driver_version: str = "Unknown"
    cuda_available: bool = False
    cuda_version: str = "N/A"
    opencl_available: bool = False


@dataclass
class MemoryInfo:
    """Memory/RAM information."""
    total_mb: int = 0
    available_mb: int = 0
    used_mb: int = 0
    percent_used: float = 0.0
    type: str = "Unknown"  # DDR4, DDR5, etc.
    speed_mhz: int = 0
    channels: int = 0
    swap_total_mb: int = 0
    swap_used_mb: int = 0


@dataclass
class DiskInfo:
    """Disk/storage information."""
    device: str = "Unknown"
    mount_point: str = "Unknown"
    filesystem: str = "Unknown"
    total_gb: float = 0.0
    used_gb: float = 0.0
    free_gb: float = 0.0
    percent_used: float = 0.0
    type: str = "Unknown"  # SSD, HDD, NVMe, etc.
    model: str = "Unknown"
    is_rotational: bool = True
    read_speed_mbps: Optional[float] = None
    write_speed_mbps: Optional[float] = None


@dataclass
class OSInfo:
    """Operating system information."""
    name: str = "Unknown"
    version: str = "Unknown"
    codename: str = "Unknown"
    kernel: str = "Unknown"
    architecture: str = "Unknown"
    distribution: str = "Unknown"
    desktop_environment: str = "Unknown"
    language: str = "Unknown"
    timezone: str = "Unknown"


@dataclass
class ContainerInfo:
    """Container/virtualization environment."""
    is_container: bool = False
    container_type: str = "None"  # docker, podman, lxc, etc.
    is_vm: bool = False
    vm_hypervisor: str = "None"  # kvm, vmware, virtualbox, etc.
    cgroup_limits: Dict[str, Any] = field(default_factory=dict)


@dataclass
class NetworkInfo:
    """Network information."""
    hostname: str = "Unknown"
    ip_address: str = "Unknown"
    mac_address: str = "Unknown"
    interface_name: str = "Unknown"
    is_wifi: bool = False


@dataclass
class SystemInfo:
    """Complete system information."""
    collection_time: str = field(default_factory=lambda: datetime.now().isoformat())
    platform: str = field(default_factory=platform_module.platform)
    python_version: str = field(default_factory=platform_module.python_version)
    cpu: CPUInfo = field(default_factory=CPUInfo)
    gpu: List[GPUInfo] = field(default_factory=list)
    memory: MemoryInfo = field(default_factory=MemoryInfo)
    disks: List[DiskInfo] = field(default_factory=list)
    os: OSInfo = field(default_factory=OSInfo)
    container: ContainerInfo = field(default_factory=ContainerInfo)
    network: NetworkInfo = field(default_factory=NetworkInfo)
    environment_variables: Dict[str, str] = field(default_factory=dict)
    benchmark_metadata: Dict[str, Any] = field(default_factory=dict)


class SystemInfoCollector:
    """Collects comprehensive system information."""

    def __init__(self):
        self.system = platform_module.system().lower()

    def collect_all(self) -> SystemInfo:
        """Collect all system information."""
        info = SystemInfo()

        info.cpu = self._collect_cpu_info()
        info.gpu = self._collect_gpu_info()
        info.memory = self._collect_memory_info()
        info.disks = self._collect_disk_info()
        info.os = self._collect_os_info()
        info.container = self._detect_container()
        info.network = self._collect_network_info()
        info.environment_variables = self._collect_relevant_env_vars()

        return info

    def _collect_cpu_info(self) -> CPUInfo:
        """Collect CPU information."""
        cpu = CPUInfo()

        try:
            # Basic CPU info
            cpu.architecture = platform_module.machine()
            cpu.logical_cores = os.cpu_count() or 0

            if self.system == "linux":
                cpu = self._collect_cpu_info_linux(cpu)
            elif self.system == "darwin":
                cpu = self._collect_cpu_info_macos(cpu)
            elif self.system == "windows":
                cpu = self._collect_cpu_info_windows(cpu)

        except Exception as e:
            print(f"Warning: Could not collect full CPU info: {e}")

        return cpu

    def _collect_cpu_info_linux(self, cpu: CPUInfo) -> CPUInfo:
        """Collect CPU info on Linux."""
        try:
            # Read /proc/cpuinfo
            with open('/proc/cpuinfo', 'r') as f:
                cpuinfo = f.read()

            # Vendor and model
            if 'vendor_id' in cpuinfo:
                vendor_match = re.search(r'vendor_id\s*:\s*(\S+)', cpuinfo)
                if vendor_match:
                    cpu.vendor = vendor_match.group(1)

            if 'model name' in cpuinfo:
                model_match = re.search(r'model name\s*:\s*(.+)', cpuinfo)
                if model_match:
                    cpu.model = model_match.group(1).strip()

            # Physical cores (unique CPU entries)
            physical_ids = set(re.findall(r'physical id\s*:\s*(\d+)', cpuinfo))
            cpu_ids = set(re.findall(r'cpu cores\s*:\s*(\d+)', cpuinfo))
            if physical_ids and cpu_ids:
                cpu.physical_cores = len(physical_ids) * int(list(cpu_ids)[0])
            else:
                cpu.physical_cores = cpu.logical_cores

            # Threads per core
            if cpu.physical_cores > 0:
                cpu.threads_per_core = cpu.logical_cores // cpu.physical_cores

            # CPU flags
            flags_match = re.search(r'flags\s*:\s*(.+)', cpuinfo)
            if flags_match:
                cpu.flags = flags_match.group(1).split()

            # Cache sizes
            cache_matches = re.findall(r'cache size\s*:\s*(\d+)\s*KB', cpuinfo)
            if cache_matches:
                cpu.cache_l3_kb = int(cache_matches[0])

            # Frequency
            try:
                with open('/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq', 'r') as f:
                    cpu.max_frequency_mhz = int(f.read().strip()) / 1000
            except:
                freq_match = re.search(r'cpu MHz\s*:\s*(\d+\.?\d*)', cpuinfo)
                if freq_match:
                    cpu.base_frequency_mhz = float(freq_match.group(1))

            # Virtualization detection
            if 'hypervisor' in cpuinfo.lower():
                cpu.virtualization = "Hypervisor detected"
            elif 'vmx' in cpu.flags or 'svm' in cpu.flags:
                cpu.virtualization = "VMX/SVM capable"

        except Exception as e:
            print(f"Warning: Linux CPU info collection failed: {e}")

        return cpu

    def _collect_cpu_info_macos(self, cpu: CPUInfo) -> CPUInfo:
        """Collect CPU info on macOS."""
        try:
            # Get CPU brand
            result = subprocess.run(['sysctl', '-n', 'machdep.cpu.brand_string'],
                                  capture_output=True, text=True)
            if result.returncode == 0:
                cpu.model = result.stdout.strip()
                if 'Intel' in cpu.model:
                    cpu.vendor = 'GenuineIntel'
                elif 'Apple' in cpu.model:
                    cpu.vendor = 'Apple'

            # Core counts
            result = subprocess.run(['sysctl', '-n', 'hw.physicalcpu'],
                                  capture_output=True, text=True)
            if result.returncode == 0:
                cpu.physical_cores = int(result.stdout.strip())

            result = subprocess.run(['sysctl', '-n', 'hw.logicalcpu'],
                                  capture_output=True, text=True)
            if result.returncode == 0:
                cpu.logical_cores = int(result.stdout.strip())

            if cpu.physical_cores > 0:
                cpu.threads_per_core = cpu.logical_cores // cpu.physical_cores

            # Cache
            result = subprocess.run(['sysctl', '-n', 'hw.l3cachesize'],
                                  capture_output=True, text=True)
            if result.returncode == 0:
                cpu.cache_l3_kb = int(result.stdout.strip()) // 1024

            # Frequency
            result = subprocess.run(['sysctl', '-n', 'hw.cpufrequency_max'],
                                  capture_output=True, text=True)
            if result.returncode == 0:
                cpu.max_frequency_mhz = int(result.stdout.strip()) / 1_000_000

        except Exception as e:
            print(f"Warning: macOS CPU info collection failed: {e}")

        return cpu

    def _collect_cpu_info_windows(self, cpu: CPUInfo) -> CPUInfo:
        """Collect CPU info on Windows."""
        try:
            import wmi
            c = wmi.WMI()

            for processor in c.Win32_Processor():
                cpu.model = processor.Name
                cpu.vendor = processor.Manufacturer
                cpu.logical_cores = processor.NumberOfLogicalProcessors
                cpu.physical_cores = processor.NumberOfCores
                cpu.base_frequency_mhz = processor.MaxClockSpeed

                if cpu.physical_cores > 0:
                    cpu.threads_per_core = cpu.logical_cores // cpu.physical_cores

        except Exception as e:
            print(f"Warning: Windows CPU info collection failed: {e}")

        return cpu

    def _collect_gpu_info(self) -> List[GPUInfo]:
        """Collect GPU information."""
        gpus = []

        try:
            # Try nvidia-smi first
            result = subprocess.run(['nvidia-smi', '--query-gpu=name,memory.total,driver_version',
                                   '--format=csv,noheader'],
                                  capture_output=True, text=True)
            if result.returncode == 0:
                for line in result.stdout.strip().split('\n'):
                    parts = [p.strip() for p in line.split(',')]
                    if len(parts) >= 3:
                        gpu = GPUInfo()
                        gpu.vendor = "NVIDIA"
                        gpu.model = parts[0]
                        # Parse memory (e.g., "8192 MiB")
                        mem_match = re.search(r'(\d+)', parts[1])
                        if mem_match:
                            gpu.vram_mb = int(mem_match.group(1))
                        gpu.driver_version = parts[2]
                        gpu.cuda_available = True

                        # Get CUDA version
                        cuda_result = subprocess.run(['nvcc', '--version'],
                                                    capture_output=True, text=True)
                        if cuda_result.returncode == 0:
                            cuda_match = re.search(r'release (\d+\.\d+)', cuda_result.stdout)
                            if cuda_match:
                                gpu.cuda_version = cuda_match.group(1)

                        gpus.append(gpu)

        except FileNotFoundError:
            pass
        except Exception as e:
            print(f"Warning: GPU info collection failed: {e}")

        # Check for Intel/AMD GPUs on Linux
        if self.system == "linux" and not gpus:
            try:
                result = subprocess.run(['lspci'], capture_output=True, text=True)
                if 'VGA' in result.stdout or 'Display' in result.stdout:
                    for line in result.stdout.split('\n'):
                        if 'VGA' in line or 'Display' in line or '3D' in line:
                            gpu = GPUInfo()
                            if 'Intel' in line:
                                gpu.vendor = "Intel"
                            elif 'AMD' in line or 'ATI' in line:
                                gpu.vendor = "AMD"
                            gpu.model = line.split(':')[-1].strip()
                            gpus.append(gpu)
            except:
                pass

        return gpus if gpus else [GPUInfo()]

    def _collect_memory_info(self) -> MemoryInfo:
        """Collect memory information."""
        mem = MemoryInfo()

        try:
            import psutil
            vm = psutil.virtual_memory()
            mem.total_mb = vm.total // (1024 * 1024)
            mem.available_mb = vm.available // (1024 * 1024)
            mem.used_mb = vm.used // (1024 * 1024)
            mem.percent_used = vm.percent

            swap = psutil.swap_memory()
            mem.swap_total_mb = swap.total // (1024 * 1024)
            mem.swap_used_mb = swap.used // (1024 * 1024)

            # Try to get memory type and speed on Linux
            if self.system == "linux":
                try:
                    result = subprocess.run(['dmidecode', '-t', 'memory'],
                                          capture_output=True, text=True)
                    if result.returncode == 0:
                        type_match = re.search(r'Type:\s*(DDR\d)', result.stdout)
                        if type_match:
                            mem.type = type_match.group(1)
                        speed_match = re.search(r'Speed:\s*(\d+)\s*MT/s', result.stdout)
                        if speed_match:
                            mem.speed_mhz = int(speed_match.group(1))
                except:
                    pass

        except ImportError:
            print("Warning: psutil not available, limited memory info")
        except Exception as e:
            print(f"Warning: Memory info collection failed: {e}")

        return mem

    def _collect_disk_info(self) -> List[DiskInfo]:
        """Collect disk information."""
        disks = []

        try:
            import psutil
            partitions = psutil.disk_partitions(all=False)

            for part in partitions:
                try:
                    usage = psutil.disk_usage(part.mountpoint)
                    disk = DiskInfo()
                    disk.device = part.device
                    disk.mount_point = part.mountpoint
                    disk.filesystem = part.fstype
                    disk.total_gb = usage.total / (1024**3)
                    disk.used_gb = usage.used / (1024**3)
                    disk.free_gb = usage.free / (1024**3)
                    disk.percent_used = (usage.used / usage.total) * 100

                    # Detect disk type
                    disk.type, disk.is_rotational = self._detect_disk_type(part.device)

                    disks.append(disk)
                except:
                    pass

        except ImportError:
            print("Warning: psutil not available, limited disk info")
        except Exception as e:
            print(f"Warning: Disk info collection failed: {e}")

        return disks if disks else [DiskInfo()]

    def _detect_disk_type(self, device: str) -> Tuple[str, bool]:
        """Detect if disk is SSD or HDD."""
        try:
            if self.system == "linux":
                # Check rotational flag
                base_device = Path(device).name
                if base_device.startswith('nvme'):
                    return "NVMe SSD", False

                rotational_path = f"/sys/block/{base_device}/queue/rotational"
                if Path(rotational_path).exists():
                    with open(rotational_path, 'r') as f:
                        is_rotational = f.read().strip() == '1'
                        return ("HDD", True) if is_rotational else ("SSD", False)

        except:
            pass

        return "Unknown", True

    def _collect_os_info(self) -> OSInfo:
        """Collect OS information."""
        os_info = OSInfo()

        try:
            os_info.name = platform_module.system()
            os_info.version = platform_module.version()
            os_info.architecture = platform_module.machine()

            if self.system == "linux":
                # Try to get distribution info
                try:
                    import distro
                    os_info.distribution = distro.name(pretty=True)
                    os_info.codename = distro.codename()
                except:
                    # Fallback to /etc/os-release
                    if Path('/etc/os-release').exists():
                        with open('/etc/os-release', 'r') as f:
                            content = f.read()
                            name_match = re.search(r'PRETTY_NAME="([^"]+)"', content)
                            if name_match:
                                os_info.distribution = name_match.group(1)

                # Kernel version
                os_info.kernel = platform_module.release()

            elif self.system == "darwin":
                os_info.distribution = "macOS"
                result = subprocess.run(['sw_vers', '-productVersion'],
                                      capture_output=True, text=True)
                if result.returncode == 0:
                    os_info.version = result.stdout.strip()

            elif self.system == "windows":
                os_info.distribution = "Windows"
                os_info.version = platform_module.win32_ver()[1]

            # Timezone
            try:
                result = subprocess.run(['timedatectl', 'show', '--property=Timezone'],
                                      capture_output=True, text=True)
                if result.returncode == 0:
                    os_info.timezone = result.stdout.strip().split('=')[-1]
            except:
                pass

        except Exception as e:
            print(f"Warning: OS info collection failed: {e}")

        return os_info

    def _detect_container(self) -> ContainerInfo:
        """Detect if running in container/VM."""
        container = ContainerInfo()

        try:
            # Check for Docker
            if Path('/.dockerenv').exists():
                container.is_container = True
                container.container_type = "Docker"
            elif Path('/run/.containerenv').exists():
                container.is_container = True
                container.container_type = "Podman"

            # Check cgroup
            if Path('/proc/1/cgroup').exists():
                with open('/proc/1/cgroup', 'r') as f:
                    cgroup_content = f.read()
                    if 'docker' in cgroup_content:
                        container.is_container = True
                        container.container_type = "Docker"
                    elif 'lxc' in cgroup_content:
                        container.is_container = True
                        container.container_type = "LXC"
                    elif 'kubepods' in cgroup_content:
                        container.is_container = True
                        container.container_type = "Kubernetes"

            # Check for VM
            try:
                result = subprocess.run(['systemd-detect-virt'],
                                      capture_output=True, text=True)
                if result.returncode == 0:
                    virt = result.stdout.strip()
                    if virt != 'none':
                        container.is_vm = True
                        container.vm_hypervisor = virt
            except:
                pass

            # Get cgroup limits if in container
            if container.is_container:
                try:
                    with open('/sys/fs/cgroup/memory/memory.limit_in_bytes', 'r') as f:
                        mem_limit = int(f.read().strip())
                        if mem_limit < 9223372036854771712:  # Not unlimited
                            container.cgroup_limits['memory_bytes'] = mem_limit
                except:
                    pass

                try:
                    with open('/sys/fs/cgroup/cpu/cpu.cfs_quota_us', 'r') as f:
                        cpu_quota = int(f.read().strip())
                        if cpu_quota > 0:
                            container.cgroup_limits['cpu_quota_us'] = cpu_quota
                except:
                    pass

        except Exception as e:
            print(f"Warning: Container detection failed: {e}")

        return container

    def _collect_network_info(self) -> NetworkInfo:
        """Collect network information."""
        net = NetworkInfo()

        try:
            net.hostname = socket.gethostname()

            # Get primary IP
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.connect(("8.8.8.8", 80))
                net.ip_address = s.getsockname()[0]
                s.close()
            except:
                pass

            # Try to get MAC address
            try:
                import psutil
                interfaces = psutil.net_if_addrs()
                for name, addrs in interfaces.items():
                    for addr in addrs:
                        if addr.family == psutil.AF_LINK:
                            net.mac_address = addr.address
                            net.interface_name = name
                            break
                    if net.mac_address != "Unknown":
                        break
            except:
                pass

        except Exception as e:
            print(f"Warning: Network info collection failed: {e}")

        return net

    def _collect_relevant_env_vars(self) -> Dict[str, str]:
        """Collect relevant environment variables."""
        relevant_vars = [
            'HOME', 'USER', 'HOSTNAME', 'SHELL', 'TERM',
            'PATH', 'LD_LIBRARY_PATH', 'LD_PRELOAD',
            'DOCKER_HOST', 'KUBERNETES_SERVICE_HOST',
            'CI', 'GITHUB_ACTIONS', 'GITLAB_CI', 'JENKINS_URL',
            'AWS_REGION', 'GCP_PROJECT', 'AZURE_SUBSCRIPTION_ID',
            'LANG', 'LC_ALL', 'TZ',
            'DB_HOST', 'DB_PORT', 'DB_NAME', 'DB_USER'
        ]

        env_vars = {}
        for var in relevant_vars:
            if var in os.environ:
                # Mask sensitive values
                if 'PASSWORD' in var or 'SECRET' in var or 'KEY' in var or 'TOKEN' in var:
                    env_vars[var] = '***masked***'
                else:
                    env_vars[var] = os.environ[var]

        return env_vars

    def save_to_json(self, info: SystemInfo, filepath: Path) -> Path:
        """Save system info to JSON file."""
        filepath = Path(filepath)
        filepath.parent.mkdir(parents=True, exist_ok=True)

        # Convert to dict
        data = asdict(info)

        with open(filepath, 'w') as f:
            json.dump(data, f, indent=2, default=str)

        return filepath

    def print_summary(self, info: SystemInfo):
        """Print human-readable summary."""
        print("\n" + "="*60)
        print("SYSTEM INFORMATION SUMMARY")
        print("="*60)

        print(f"\nOperating System:")
        print(f"  Name: {info.os.name} {info.os.version}")
        print(f"  Distribution: {info.os.distribution}")
        print(f"  Kernel: {info.os.kernel}")
        print(f"  Architecture: {info.os.architecture}")

        print(f"\nCPU:")
        print(f"  Model: {info.cpu.model}")
        print(f"  Cores: {info.cpu.physical_cores} physical, {info.cpu.logical_cores} logical")
        print(
            f"  Frequency: {info.cpu.base_frequency_mhz:.0f} MHz (base), "
            f"{info.cpu.max_frequency_mhz:.0f} MHz (max)"
        )
        if info.cpu.virtualization != "Unknown":
            print(f"  Virtualization: {info.cpu.virtualization}")

        print(f"\nMemory:")
        print(f"  Total: {info.memory.total_mb:,} MB ({info.memory.total_mb/1024:.1f} GB)")
        print(f"  Used: {info.memory.used_mb:,} MB ({info.memory.percent_used:.1f}%)")
        print(f"  Type: {info.memory.type} @ {info.memory.speed_mhz} MHz")

        print(f"\nStorage:")
        for disk in info.disks:
            print(f"  {disk.device}: {disk.total_gb:.1f} GB ({disk.type}, {disk.filesystem})")
            print(f"    Free: {disk.free_gb:.1f} GB ({100-disk.percent_used:.1f}%)")

        if info.gpu and info.gpu[0].vendor != "Unknown":
            print(f"\nGPU:")
            for gpu in info.gpu:
                print(f"  {gpu.vendor} {gpu.model}")
                print(f"    VRAM: {gpu.vram_mb} MB")
                if gpu.cuda_available:
                    print(f"    CUDA: {gpu.cuda_version}")

        print(f"\nContainer/VM:")
        if info.container.is_container:
            print(f"  Container: {info.container.container_type}")
        elif info.container.is_vm:
            print(f"  VM: {info.container.vm_hypervisor}")
        else:
            print(f"  Bare metal")

        print("="*60)


def main():
    """CLI entry point."""
    import argparse

    parser = argparse.ArgumentParser(description='System Information Collector')
    parser.add_argument('--output', '-o', type=Path, default=Path('system-info.json'),
                       help='Output JSON file path')
    parser.add_argument('--quiet', '-q', action='store_true',
                       help='Suppress output')

    args = parser.parse_args()

    collector = SystemInfoCollector()
    info = collector.collect_all()

    # Save to JSON
    output_path = collector.save_to_json(info, args.output)

    if not args.quiet:
        collector.print_summary(info)
        print(f"\nSystem information saved to: {output_path}")

    return output_path


if __name__ == '__main__':
    main()
