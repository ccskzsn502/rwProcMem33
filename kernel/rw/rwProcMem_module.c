#include "rwProcMem_module.h"
#include "cfi_bypass.h"
#include "proc_search.h"

#define MY_TASK_COMM_LEN 16

#pragma pack(push,1)
struct init_device_info {
	int pid;
	int tgid;
	char my_name[MY_TASK_COMM_LEN + 1];
	char my_auxv[1024];
	int my_auxv_size;
};
struct arg_info {
	uint64_t arg_start;
	uint64_t arg_end;
};
#pragma pack(pop)

static ssize_t OnCmdInitDeviceInfo(struct ioctl_request *hdr, char __user* buf) {
	long err = 0;
	struct init_device_info* pinit_device_info = (struct init_device_info*)x_kmalloc(sizeof(struct init_device_info), GFP_KERNEL);
	if (!pinit_device_info) {
		return -ENOMEM;
	}
	printk_debug(KERN_INFO "CMD_INIT_DEVICE_INFO\n");
	memset(pinit_device_info, 0, sizeof(struct init_device_info));
	if (x_copy_from_user((void*)pinit_device_info, (void*)buf, sizeof(struct init_device_info)) == 0) {
		printk_debug(KERN_INFO "my_name:%s\n", pinit_device_info->my_name);
		printk_debug(KERN_INFO "pid:%d, tgid:%d\n", pinit_device_info->pid, pinit_device_info->tgid);
		printk_debug(KERN_INFO "my_auxv_size:%d\n", pinit_device_info->my_auxv_size);
		
		do {
			err = init_mmap_lock_offset();
			if(err) { break; }
			err = init_map_count_offset();
			if(err) { break; }
			err = init_proc_cmdline_offset(&pinit_device_info->my_auxv[0], pinit_device_info->my_auxv_size);
			if(err) { break; }
			err = init_proc_root_offset(pinit_device_info->my_name);
			if(err) { break; }
			err = init_task_next_offset();
			if(err) { break; }
			err = init_task_pid_offset(pinit_device_info->pid, pinit_device_info->tgid);
		} while(0);
	} else {
		err = -EINVAL;
	}
	kfree(pinit_device_info);
	return err;
}

static ssize_t OnCmdOpenProcess(struct ioctl_request *hdr, char __user* buf) {
	uint64_t pid = hdr->param1, handle = 0;
	struct pid * proc_pid_struct = NULL;
	printk_debug(KERN_INFO "CMD_OPEN_PROCESS\n");

	printk_debug(KERN_INFO "pid:%llu,size:%ld\n", pid, sizeof(pid));

	proc_pid_struct = get_proc_pid_struct(pid);
	printk_debug(KERN_INFO "proc_pid_struct *:0x%p\n", (void*)proc_pid_struct);
	if (!proc_pid_struct) {
		return -EINVAL;
	}
	handle = proc_handle_alloc(proc_pid_struct);
	if (!handle) {
		release_proc_pid_struct(proc_pid_struct);
		return -ENFILE;
	}

	printk_debug(KERN_INFO "handle:%llu,size:%ld\n", handle, sizeof(handle));
	if (!!x_copy_to_user((void*)buf, (void*)&handle, sizeof(handle))) {
		struct pid *p = proc_handle_put_remove(handle);
		if (p)
			release_proc_pid_struct(p);
		return -EINVAL;
	}
	return 0;
}

static ssize_t OnCmdCloseProcess(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct;
	printk_debug(KERN_INFO "CMD_CLOSE_PROCESS\n");
	printk_debug(KERN_INFO "handle:%llu\n", (unsigned long long)hdr->param1);
	proc_pid_struct = proc_handle_put_remove(hdr->param1);
	if (!proc_pid_struct)
		return -EINVAL;
	release_proc_pid_struct(proc_pid_struct);
	return 0;
}

static ssize_t OnCmdReadProcessMemory(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = proc_handle_get(hdr->param1);
	size_t proc_virt_addr = (size_t)hdr->param2;
	bool is_force_read = hdr->param3 == 1 ? true : false;
	size_t size = (size_t)hdr->buf_size;
	ssize_t ret;
	size_t end_addr;

	if (!proc_pid_struct)
		return -EINVAL;
	if (size == 0 || size > (16UL * 1024UL * 1024UL)) {
		release_proc_pid_struct(proc_pid_struct);
		return -EINVAL;
	}
	end_addr = proc_virt_addr + size;
	if (end_addr < proc_virt_addr) {
		release_proc_pid_struct(proc_pid_struct);
		return -EINVAL;
	}
	ret = mirror_copy_from_remote(proc_pid_struct, proc_virt_addr, buf, size, is_force_read);
	release_proc_pid_struct(proc_pid_struct);
	return ret;
}

static ssize_t OnCmdWriteProcessMemory(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = proc_handle_get(hdr->param1);
	size_t proc_virt_addr = (size_t)hdr->param2;
	bool is_force_write = hdr->param3 == 1 ? true : false;
	size_t size = (size_t)hdr->buf_size;
	ssize_t ret;
	size_t end_addr;

	if (!proc_pid_struct)
		return -EINVAL;
	if (size == 0 || size > (16UL * 1024UL * 1024UL)) {
		release_proc_pid_struct(proc_pid_struct);
		return -EINVAL;
	}
	end_addr = proc_virt_addr + size;
	if (end_addr < proc_virt_addr) {
		release_proc_pid_struct(proc_pid_struct);
		return -EINVAL;
	}
	ret = mirror_copy_to_remote(proc_pid_struct, proc_virt_addr, buf, size, is_force_write);
	release_proc_pid_struct(proc_pid_struct);
	return ret;
}

static ssize_t OnCmdGetProcessMapsCount(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = proc_handle_get(hdr->param1);
	if (!proc_pid_struct)
		return -EINVAL;
	printk_debug(KERN_INFO "CMD_GET_PROCESS_MAPS_COUNT\n");
	printk_debug(KERN_INFO "proc_pid_struct*:0x%p, size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));

	release_proc_pid_struct(proc_pid_struct);
	return get_proc_map_count(proc_pid_struct);
}

static ssize_t OnCmdGetProcessMapsList(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = proc_handle_get(hdr->param1);
	if (!proc_pid_struct)
		return -EINVAL;
	printk_debug(KERN_INFO "CMD_GET_PROCESS_MAPS_LIST\n");
	printk_debug(KERN_INFO "proc_pid_struct*:0x%p,size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));
	printk_debug(KERN_INFO "buf_size:%llu\n", hdr->buf_size);
	release_proc_pid_struct(proc_pid_struct);
	return get_proc_maps_list(false, proc_pid_struct, (void*)(buf), hdr->buf_size - 1);
}

static ssize_t OnCmdCheckProcessPhyAddr(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = proc_handle_get(hdr->param1);
	if (!proc_pid_struct)
		return -EINVAL;
	size_t proc_virt_addr = (size_t)hdr->param2;
	pte_t *pte;
	printk_debug(KERN_INFO "CMD_CHECK_PROCESS_ADDR_PHY\n");
	printk_debug(KERN_INFO "proc_pid_struct *:0x%p,size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));
	printk_debug(KERN_INFO "proc_virt_addr :0x%zx\n", proc_virt_addr);
	if (get_proc_phy_addr(proc_pid_struct, proc_virt_addr, &pte)) {
		return 1;
	}
	release_proc_pid_struct(proc_pid_struct);
	return 0;
}

static ssize_t OnCmdGetPidList(struct ioctl_request *hdr, char __user* buf) {
	printk_debug(KERN_INFO "CMD_GET_PID_LIST\n");
	printk_debug(KERN_INFO "buf_size:%llu\n", hdr->buf_size);
	return get_proc_pid_list(false, buf, hdr->buf_size);
}

static ssize_t OnCmdSetProcessRoot(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = proc_handle_get(hdr->param1);
	if (!proc_pid_struct)
		return -EINVAL;
	printk_debug(KERN_INFO "CMD_SET_PROCESS_ROOT\n");
	printk_debug(KERN_INFO "proc_pid_struct*:0x%p,size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));
	release_proc_pid_struct(proc_pid_struct);
	return set_process_root(proc_pid_struct);
}

static ssize_t OnCmdGetProcessRss(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = proc_handle_get(hdr->param1);
	if (!proc_pid_struct)
		return -EINVAL;
	uint64_t rss = 0;
	printk_debug(KERN_INFO "CMD_GET_PROCESS_RSS\n");
	printk_debug(KERN_INFO "proc_pid_struct*:0x%p,size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));
	rss = read_proc_rss_size(proc_pid_struct);
	if (!!x_copy_to_user((void*)buf, &rss, sizeof(rss))) {
		return -EINVAL;
	}
	release_proc_pid_struct(proc_pid_struct);
	return 0;
}

static ssize_t OnCmdGetProcessCmdlineAddr(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = proc_handle_get(hdr->param1);
	if (!proc_pid_struct)
		return -EINVAL;
	size_t arg_start = 0, arg_end = 0;
	int res;
	struct arg_info aginfo = {0};
	printk_debug(KERN_INFO "CMD_GET_PROCESS_CMDLINE_ADDR\n");
	printk_debug(KERN_INFO "proc_pid_struct *:0x%p,size:%ld\n", (void*)proc_pid_struct, sizeof(proc_pid_struct));
	res = get_proc_cmdline_addr(proc_pid_struct, &arg_start, &arg_end);
	aginfo.arg_start = (uint64_t)arg_start;
	aginfo.arg_end = (uint64_t)arg_end;
	if (!!x_copy_to_user((void*)buf, &aginfo, sizeof(aginfo))) {
		return -EINVAL;
	}
	release_proc_pid_struct(proc_pid_struct);
	return res;
}

static ssize_t OnCmdHideKernelModule(struct ioctl_request *hdr, char __user* buf) {
	printk_debug(KERN_INFO "CMD_HIDE_KERNEL_MODULE\n");

	if (g_rwProcMem_devp->is_hidden_module == false) {
		g_rwProcMem_devp->is_hidden_module = true; 
		list_del_init(&__this_module.list);
		/* skip kobject_del for unload safety */
	}
	return 0;
}


static ssize_t OnCmdCreateMirrorMap(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = proc_handle_get(hdr->param1);
	uint64_t remote_addr = hdr->param2;
	uint64_t length = hdr->param3;
	pid_t pid_nr_val;
	int fd;
	int64_t out_fd;

	if (!proc_pid_struct)
		return -EINVAL;
	if (!length || length > MIRROR_MAX_SIZE ||
	    (remote_addr & (PAGE_SIZE - 1)) || (length & (PAGE_SIZE - 1))) {
		release_proc_pid_struct(proc_pid_struct);
		return -EINVAL;
	}
	pid_nr_val = pid_nr(proc_pid_struct);
	release_proc_pid_struct(proc_pid_struct);
	if (!pid_nr_val)
		return -ESRCH;
	fd = mirror_create_fd(pid_nr_val, (unsigned long)remote_addr, (unsigned long)length);
	if (fd < 0)
		return fd;
	out_fd = fd;
	if (x_copy_to_user(buf, &out_fd, sizeof(out_fd)))
		return -EFAULT;
	return 0;
}

static ssize_t OnCmdSearchProcessMemory(struct ioctl_request *hdr, char __user* buf) {
	struct pid * proc_pid_struct = proc_handle_get(hdr->param1);
	printk_debug(KERN_INFO "CMD_SEARCH_PROCESS_MEMORY\n");
	printk_debug(KERN_INFO "proc_pid_struct*:0x%p buf_size:%llu\n",
		(void*)proc_pid_struct, hdr->buf_size);
	if (!proc_pid_struct)
		return -EINVAL;
	release_proc_pid_struct(proc_pid_struct);
	return search_process_memory(proc_pid_struct, buf, (size_t)hdr->buf_size);
}

#ifdef CONFIG_MERGED_MODULE
ssize_t rw_dispatch_command(struct ioctl_request *hdr, char __user* buf)
#else
static inline ssize_t DispatchCommand(struct ioctl_request *hdr, char __user* buf)
#endif
{
	if (!capable(CAP_SYS_RAWIO) && !capable(CAP_SYS_PTRACE) && !capable(CAP_SYS_ADMIN))
		return -EPERM;
	switch (hdr->cmd) {
	case CMD_INIT_DEVICE_INFO:
		return OnCmdInitDeviceInfo(hdr, buf);
	case CMD_OPEN_PROCESS:
		return OnCmdOpenProcess(hdr, buf);
	case CMD_READ_PROCESS_MEMORY:
		return OnCmdReadProcessMemory(hdr, buf);
	case CMD_WRITE_PROCESS_MEMORY:
		return OnCmdWriteProcessMemory(hdr, buf);
	case CMD_CLOSE_PROCESS:
		return OnCmdCloseProcess(hdr, buf);
	case CMD_GET_PROCESS_MAPS_COUNT:
		return OnCmdGetProcessMapsCount(hdr, buf);
	case CMD_GET_PROCESS_MAPS_LIST:
		return OnCmdGetProcessMapsList(hdr, buf);
	case CMD_CHECK_PROCESS_ADDR_PHY:
		return OnCmdCheckProcessPhyAddr(hdr, buf);
	case CMD_GET_PID_LIST:
		return OnCmdGetPidList(hdr, buf);
	case CMD_SET_PROCESS_ROOT:
		return OnCmdSetProcessRoot(hdr, buf);
	case CMD_GET_PROCESS_RSS:
		return OnCmdGetProcessRss(hdr, buf);
	case CMD_GET_PROCESS_CMDLINE_ADDR:
		return OnCmdGetProcessCmdlineAddr(hdr, buf);
	case CMD_HIDE_KERNEL_MODULE:
		return OnCmdHideKernelModule(hdr, buf);
	case CMD_SEARCH_PROCESS_MEMORY:
		return OnCmdSearchProcessMemory(hdr, buf);
	case CMD_CREATE_MIRROR_MAP:
		return OnCmdCreateMirrorMap(hdr, buf);
	default:
		return -EINVAL;
	}
	return -EINVAL;
}

#ifndef CONFIG_MERGED_MODULE
static ssize_t rwProcMem_read(struct file* filp,
                              char __user* buf,
                              size_t size,
                              loff_t* ppos) {
    struct ioctl_request hdr = {0};
    size_t header_size = sizeof(hdr);

    if (size < header_size) {
        return -EINVAL;
    }

    if (x_copy_from_user(&hdr, buf, header_size)) {
        return -EFAULT;
    }

    if (size < header_size + hdr.buf_size) {
        return -EINVAL;
    }

    return DispatchCommand(&hdr, buf + header_size);
}
#endif

/* Core init without sole module entry / sole proc ownership (merged uses this). */
int rw_subsystem_init(void) {
#ifdef CONFIG_SAFE_MINIMAL_INIT
	printk(KERN_EMERG "rwProcMem: SAFE_MINIMAL_INIT hello %s\n", CONFIG_PROC_NODE_AUTH_KEY);
	return 0;
#else
	{
		int mret = mirror_init();
		if (mret) {
			printk(KERN_EMERG "rwProcMem: mirror_init failed %d\n", mret);
			return mret;
		}
	}
	g_rwProcMem_devp = x_kmalloc(sizeof(struct rwProcMemDev), GFP_KERNEL);
	if (!g_rwProcMem_devp) {
		printk(KERN_EMERG "rwProcMem: kmalloc failed\n");
		mirror_exit();
		return -ENOMEM;
	}
	memset(g_rwProcMem_devp, 0, sizeof(struct rwProcMemDev));

#if defined(CONFIG_USE_PROC_FILE_NODE) && !defined(CONFIG_MERGED_MODULE)
	g_rwProcMem_devp->proc_parent = proc_mkdir(CONFIG_PROC_NODE_AUTH_KEY, NULL);
	if (!g_rwProcMem_devp->proc_parent) {
		printk(KERN_EMERG "rwProcMem: proc_mkdir failed for %s\n", CONFIG_PROC_NODE_AUTH_KEY);
		kfree(g_rwProcMem_devp);
		g_rwProcMem_devp = NULL;
		return -ENOMEM;
	}
	g_rwProcMem_devp->proc_entry = proc_create(CONFIG_PROC_NODE_AUTH_KEY, S_IRUSR | S_IWUSR, g_rwProcMem_devp->proc_parent, &rwProcMem_proc_ops);
	if (!g_rwProcMem_devp->proc_entry) {
		printk(KERN_EMERG "rwProcMem: proc_create failed for %s\n", CONFIG_PROC_NODE_AUTH_KEY);
		proc_remove(g_rwProcMem_devp->proc_parent);
		g_rwProcMem_devp->proc_parent = NULL;
		kfree(g_rwProcMem_devp);
		g_rwProcMem_devp = NULL;
		return -ENOMEM;
	}
#ifdef CONFIG_HIDE_PROCFS_DIR
	if (!start_hide_procfs_dir(CONFIG_PROC_NODE_AUTH_KEY)) {
		printk(KERN_EMERG "rwProcMem: hide_procfs_dir failed, continue without hide\n");
	}
#endif
	printk(KERN_EMERG "rwProcMem: proc ready /proc/%s/%s\n",
	       CONFIG_PROC_NODE_AUTH_KEY, CONFIG_PROC_NODE_AUTH_KEY);
#endif

	printk(KERN_EMERG "rwProcMem: subsystem ready\n");
	return 0;
#endif
}

void rw_subsystem_exit(void) {
#ifdef CONFIG_SAFE_MINIMAL_INIT
	printk(KERN_EMERG "rwProcMem: SAFE_MINIMAL_INIT goodbye\n");
	return;
#else
#if defined(CONFIG_USE_PROC_FILE_NODE) && !defined(CONFIG_MERGED_MODULE)
	if (g_rwProcMem_devp) {
		if (g_rwProcMem_devp->proc_entry) {
			proc_remove(g_rwProcMem_devp->proc_entry);
			g_rwProcMem_devp->proc_entry = NULL;
		}
		if (g_rwProcMem_devp->proc_parent) {
			proc_remove(g_rwProcMem_devp->proc_parent);
			g_rwProcMem_devp->proc_parent = NULL;
		}
	}
#ifdef CONFIG_HIDE_PROCFS_DIR
	stop_hide_procfs_dir();
#endif
#endif
	if (g_rwProcMem_devp) {
		kfree(g_rwProcMem_devp);
		g_rwProcMem_devp = NULL;
	}
	proc_handle_table_destroy();
	mirror_exit();
	printk(KERN_EMERG "rwProcMem: subsystem exit\n");
#endif
}

#ifndef CONFIG_MERGED_MODULE
static int rwProcMem_dev_init(void) {
	/* Match lsnbm: neutralize old CFI slowpath before any indirect kernel calls. */
	if (!rw_bypass_cfi()) {
		printk(KERN_EMERG "rwProcMem: CFI bypass failed\n");
		/* Continue: KP may already bypass kCFI; still try to load. */
	}
	return rw_subsystem_init();
}

static void rwProcMem_dev_exit(void) {
	rw_subsystem_exit();
}

int __init init_module(void) {
    return rwProcMem_dev_init();
}

void __exit cleanup_module(void) {
    rwProcMem_dev_exit();
}

#ifndef CONFIG_MODULE_GUIDE_ENTRY
//Hook:__cfi_check_fn
unsigned char* __check_(unsigned char* result, void *ptr, void *diag)
{
	printk_debug(KERN_EMERG "my__cfi_check_fn!!!\n");
	return result;
}

//Hook:__cfi_check_fail
unsigned char * __check_fail_(unsigned char *result)
{
	printk_debug(KERN_EMERG "my__cfi_check_fail!!!\n");
	return result;
}
#endif

/* Do not define a local canary; use kernel's __stack_chk_guard when present. */
extern unsigned long __stack_chk_guard __attribute__((weak));

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux");
MODULE_DESCRIPTION("Linux default module");
#endif /* !CONFIG_MERGED_MODULE */

