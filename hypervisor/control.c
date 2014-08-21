/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) Siemens AG, 2013
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <jailhouse/entry.h>
#include <jailhouse/control.h>
#include <jailhouse/printk.h>
#include <jailhouse/paging.h>
#include <jailhouse/processor.h>
#include <jailhouse/string.h>
#include <jailhouse/utils.h>
#include <asm/bitops.h>
#include <asm/spinlock.h>

enum msg_type {MSG_REQUEST, MSG_INFORMATION};
enum failure_mode {ABORT_ON_ERROR, WARN_ON_ERROR};
enum management_task {CELL_START, CELL_SET_LOADABLE, CELL_DESTROY};

struct jailhouse_system *system_config;

static DEFINE_SPINLOCK(shutdown_lock);
static unsigned int num_cells = 1;

#define for_each_cell(c)	for ((c) = &root_cell; (c); (c) = (c)->next)
#define for_each_non_root_cell(c) \
	for ((c) = root_cell.next; (c); (c) = (c)->next)

unsigned int next_cpu(unsigned int cpu, struct cpu_set *cpu_set, int exception)
{
	do
		cpu++;
	while (cpu <= cpu_set->max_cpu_id &&
	       (cpu == exception || !test_bit(cpu, cpu_set->bitmap)));
	return cpu;
}

bool cpu_id_valid(unsigned long cpu_id)
{
	const unsigned long *system_cpu_set =
		jailhouse_cell_cpu_set(&system_config->root_cell);

	return (cpu_id < system_config->root_cell.cpu_set_size * 8 &&
		test_bit(cpu_id, system_cpu_set));
}

static void cell_suspend(struct cell *cell, struct per_cpu *cpu_data)
{
	unsigned int cpu;

	for_each_cpu_except(cpu, cell->cpu_set, cpu_data->cpu_id)
		arch_suspend_cpu(cpu);
}

static void cell_resume(struct per_cpu *cpu_data)
{
	unsigned int cpu;

	for_each_cpu_except(cpu, cpu_data->cell->cpu_set, cpu_data->cpu_id)
		arch_resume_cpu(cpu);
}

/**
 * cell_send_message - Deliver a message to cell and wait for the reply
 * @cell: target cell
 * @message: message code to be sent (JAILHOUSE_MSG_*)
 * @type: message type, defines the valid replies
 *
 * Returns true if a request message was approved or reception of an
 * information message was acknowledged by the target cell. It also return true
 * of the target cell does not support a communication region, is shut down or
 * in failed state. Return false on request denial or invalid replies.
 */
static bool cell_send_message(struct cell *cell, u32 message,
			      enum msg_type type)
{
	if (cell->config->flags & JAILHOUSE_CELL_PASSIVE_COMMREG)
		return true;

	jailhouse_send_msg_to_cell(&cell->comm_page.comm_region, message);

	while (1) {
		u32 reply = cell->comm_page.comm_region.reply_from_cell;
		u32 cell_state = cell->comm_page.comm_region.cell_state;

		if (cell_state == JAILHOUSE_CELL_SHUT_DOWN ||
		    cell_state == JAILHOUSE_CELL_FAILED)
			return true;

		if ((type == MSG_REQUEST &&
		     reply == JAILHOUSE_MSG_REQUEST_APPROVED) ||
		    (type == MSG_INFORMATION &&
		     reply == JAILHOUSE_MSG_RECEIVED))
			return true;

		if (reply != JAILHOUSE_MSG_NONE)
			return false;

		cpu_relax();
	}
}

static bool cell_reconfig_ok(struct cell *excluded_cell)
{
	struct cell *cell;

	for_each_non_root_cell(cell)
		if (cell != excluded_cell &&
		    cell->comm_page.comm_region.cell_state ==
				JAILHOUSE_CELL_RUNNING_LOCKED)
			return false;
	return true;
}

static void cell_reconfig_completed(void)
{
	struct cell *cell;

	for_each_non_root_cell(cell)
		cell_send_message(cell, JAILHOUSE_MSG_RECONFIG_COMPLETED,
				  MSG_INFORMATION);
}

static unsigned int get_free_cell_id(void)
{
	unsigned int id = 0;
	struct cell *cell;

retry:
	for_each_cell(cell)
		if (cell->id == id) {
			id++;
			goto retry;
		}

	return id;
}

/* cell must be zero-initialized */
int cell_init(struct cell *cell)
{
	const unsigned long *config_cpu_set =
		jailhouse_cell_cpu_set(cell->config);
	unsigned long cpu_set_size = cell->config->cpu_set_size;
	struct cpu_set *cpu_set;

	cell->id = get_free_cell_id();

	if (cpu_set_size > PAGE_SIZE)
		return -EINVAL;
	if (cpu_set_size > sizeof(cell->small_cpu_set.bitmap)) {
		cpu_set = page_alloc(&mem_pool, 1);
		if (!cpu_set)
			return -ENOMEM;
	} else {
		cpu_set = &cell->small_cpu_set;
	}
	cpu_set->max_cpu_id = cpu_set_size * 8 - 1;
	memcpy(cpu_set->bitmap, config_cpu_set, cpu_set_size);

	cell->cpu_set = cpu_set;

	return 0;
}

static void destroy_cpu_set(struct cell *cell)
{
	if (cell->cpu_set != &cell->small_cpu_set)
		page_free(&mem_pool, cell->cpu_set, 1);
}

int check_mem_regions(const struct jailhouse_cell_desc *config)
{
	const struct jailhouse_memory *mem =
		jailhouse_cell_mem_regions(config);
	unsigned int n;

	for (n = 0; n < config->num_memory_regions; n++, mem++) {
		if (mem->phys_start & ~PAGE_MASK ||
		    mem->virt_start & ~PAGE_MASK ||
		    mem->size & ~PAGE_MASK ||
		    mem->flags & ~JAILHOUSE_MEM_VALID_FLAGS) {
			printk("FATAL: Invalid memory bar (%p, %p, %p, %x)\n",
			       mem->phys_start, mem->virt_start, mem->size,
			       mem->flags);
			return -EINVAL;
		}
	}
	return 0;
}

static bool address_in_region(unsigned long addr,
			      const struct jailhouse_memory *region)
{
	return addr >= region->phys_start &&
	       addr < (region->phys_start + region->size);
}

static int unmap_from_root_cell(const struct jailhouse_memory *mem)
{
	/*
	 * arch_unmap_memory_region uses the virtual address of the memory
	 * region. As only the root cell has a guaranteed 1:1 mapping, make a
	 * copy where we ensure this.
	 */
	struct jailhouse_memory tmp = *mem;

	tmp.virt_start = tmp.phys_start;
	return arch_unmap_memory_region(&root_cell, &tmp);
}

static int remap_to_root_cell(const struct jailhouse_memory *mem,
			      enum failure_mode mode)
{
	const struct jailhouse_memory *root_mem =
		jailhouse_cell_mem_regions(root_cell.config);
	struct jailhouse_memory overlap;
	unsigned int n;
	int err = 0;

	for (n = 0; n < root_cell.config->num_memory_regions;
	     n++, root_mem++) {
		if (address_in_region(mem->phys_start, root_mem)) {
			overlap.phys_start = mem->phys_start;
			overlap.size = root_mem->size -
				(overlap.phys_start - root_mem->phys_start);
			if (overlap.size > mem->size)
				overlap.size = mem->size;
		} else if (address_in_region(root_mem->phys_start, mem)) {
			overlap.phys_start = root_mem->phys_start;
			overlap.size = mem->size -
				(overlap.phys_start - mem->phys_start);
			if (overlap.size > root_mem->size)
				overlap.size = root_mem->size;
		} else
			continue;

		overlap.virt_start = root_mem->virt_start +
			overlap.phys_start - root_mem->phys_start;
		overlap.flags = root_mem->flags;

		err = arch_map_memory_region(&root_cell, &overlap);
		if (err) {
			if (mode == ABORT_ON_ERROR)
				break;
			printk("WARNING: Failed to re-assign memory region "
			       "to root cell\n");
		}
	}
	return err;
}

static void cell_destroy_internal(struct per_cpu *cpu_data, struct cell *cell)
{
	const struct jailhouse_memory *mem =
		jailhouse_cell_mem_regions(cell->config);
	unsigned int cpu, n;

	for_each_cpu(cpu, cell->cpu_set) {
		arch_park_cpu(cpu);

		set_bit(cpu, root_cell.cpu_set->bitmap);
		per_cpu(cpu)->cell = &root_cell;
		per_cpu(cpu)->failed = false;
		memset(per_cpu(cpu)->stats, 0, sizeof(per_cpu(cpu)->stats));
	}

	for (n = 0; n < cell->config->num_memory_regions; n++, mem++) {
		/*
		 * This cannot fail. The region was mapped as a whole before,
		 * thus no hugepages need to be broken up to unmap it.
		 */
		arch_unmap_memory_region(cell, mem);
		if (!(mem->flags & JAILHOUSE_MEM_COMM_REGION))
			remap_to_root_cell(mem, WARN_ON_ERROR);
	}

	arch_cell_destroy(cpu_data, cell);

	arch_config_commit(cpu_data, cell);
}

static int cell_create(struct per_cpu *cpu_data, unsigned long config_address)
{
	unsigned long cfg_page_offs = config_address & ~PAGE_MASK;
	unsigned int cfg_pages, cell_pages, cpu, n;
	const struct jailhouse_memory *mem;
	struct jailhouse_cell_desc *cfg;
	unsigned long cfg_total_size;
	struct cell *cell, *last;
	void *cfg_mapping;
	int err;

	/* We do not support creation over non-root cells. */
	if (cpu_data->cell != &root_cell)
		return -EPERM;

	cell_suspend(&root_cell, cpu_data);

	if (!cell_reconfig_ok(NULL)) {
		err = -EPERM;
		goto err_resume;
	}

	cfg_pages = PAGE_ALIGN(sizeof(struct jailhouse_cell_desc) +
			       cfg_page_offs) / PAGE_SIZE;
	cfg_mapping = page_map_get_guest_pages(NULL, config_address, cfg_pages,
					       PAGE_READONLY_FLAGS);
	if (!cfg_mapping) {
		err = -ENOMEM;
		goto err_resume;
	}

	cfg = (struct jailhouse_cell_desc *)(cfg_mapping + cfg_page_offs);

	for_each_cell(cell)
		if (strcmp(cell->config->name, cfg->name) == 0) {
			err = -EEXIST;
			goto err_resume;
		}

	cfg_total_size = jailhouse_cell_config_size(cfg);
	cfg_pages = PAGE_ALIGN(cfg_total_size + cfg_page_offs) / PAGE_SIZE;
	if (cfg_pages > NUM_TEMPORARY_PAGES) {
		err = -E2BIG;
		goto err_resume;
	}

	if (!page_map_get_guest_pages(NULL, config_address, cfg_pages,
				      PAGE_READONLY_FLAGS)) {
		err = -ENOMEM;
		goto err_resume;
	}

	err = check_mem_regions(cfg);
	if (err)
		goto err_resume;

	cell_pages = PAGE_ALIGN(sizeof(*cell) + cfg_total_size) / PAGE_SIZE;
	cell = page_alloc(&mem_pool, cell_pages);
	if (!cell) {
		err = -ENOMEM;
		goto err_resume;
	}

	cell->data_pages = cell_pages;
	cell->config = ((void *)cell) + sizeof(*cell);
	memcpy(cell->config, cfg, cfg_total_size);

	err = cell_init(cell);
	if (err)
		goto err_free_cell;

	/* don't assign the CPU we are currently running on */
	if (cell_owns_cpu(cell, cpu_data->cpu_id)) {
		err = -EBUSY;
		goto err_free_cpu_set;
	}

	/* the root cell's cpu set must be super-set of new cell's set */
	for_each_cpu(cpu, cell->cpu_set)
		if (!cell_owns_cpu(&root_cell, cpu)) {
			err = -EBUSY;
			goto err_free_cpu_set;
		}

	err = arch_cell_create(cpu_data, cell);
	if (err)
		goto err_free_cpu_set;

	for_each_cpu(cpu, cell->cpu_set) {
		arch_park_cpu(cpu);

		clear_bit(cpu, root_cell.cpu_set->bitmap);
		per_cpu(cpu)->cell = cell;
		memset(per_cpu(cpu)->stats, 0, sizeof(per_cpu(cpu)->stats));
	}

	/*
	 * Unmap the cell's memory regions from the root cell and map them to
	 * the new cell instead.
	 */
	mem = jailhouse_cell_mem_regions(cell->config);
	for (n = 0; n < cell->config->num_memory_regions; n++, mem++) {
		/*
		 * Unmap exceptions:
		 *  - the communication region is not backed by root memory
		 */
		if (!(mem->flags & JAILHOUSE_MEM_COMM_REGION)) {
			err = unmap_from_root_cell(mem);
			if (err)
				goto err_destroy_cell;
		}

		err = arch_map_memory_region(cell, mem);
		if (err)
			goto err_destroy_cell;
	}

	arch_config_commit(cpu_data, cell);

	cell->comm_page.comm_region.cell_state = JAILHOUSE_CELL_SHUT_DOWN;

	last = &root_cell;
	while (last->next)
		last = last->next;
	last->next = cell;
	num_cells++;

	cell_reconfig_completed();

	printk("Created cell \"%s\"\n", cell->config->name);

	page_map_dump_stats("after cell creation");

	cell_resume(cpu_data);

	return cell->id;

err_destroy_cell:
	cell_destroy_internal(cpu_data, cell);
err_free_cpu_set:
	destroy_cpu_set(cell);
err_free_cell:
	page_free(&mem_pool, cell, cell_pages);

err_resume:
	cell_resume(cpu_data);

	return err;
}

static bool cell_shutdown_ok(struct cell *cell)
{
	return cell_send_message(cell, JAILHOUSE_MSG_SHUTDOWN_REQUEST,
				 MSG_REQUEST);
}

static int cell_management_prologue(enum management_task task,
				    struct per_cpu *cpu_data, unsigned long id,
				    struct cell **cell_ptr)
{
	/* We do not support management commands over non-root cells. */
	if (cpu_data->cell != &root_cell)
		return -EPERM;

	cell_suspend(&root_cell, cpu_data);

	for_each_cell(*cell_ptr)
		if ((*cell_ptr)->id == id)
			break;

	if (!*cell_ptr) {
		cell_resume(cpu_data);
		return -ENOENT;
	}

	/* root cell cannot be managed */
	if (*cell_ptr == &root_cell) {
		cell_resume(cpu_data);
		return -EINVAL;
	}

	if ((task == CELL_DESTROY && !cell_reconfig_ok(*cell_ptr)) ||
	    !cell_shutdown_ok(*cell_ptr)) {
		cell_resume(cpu_data);
		return -EPERM;
	}

	cell_suspend(*cell_ptr, cpu_data);

	return 0;
}

static int cell_start(struct per_cpu *cpu_data, unsigned long id)
{
	const struct jailhouse_memory *mem;
	unsigned int cpu, n;
	struct cell *cell;
	int err;

	err = cell_management_prologue(CELL_START, cpu_data, id, &cell);
	if (err)
		return err;

	if (cell->loadable) {
		/* unmap all loadable memory regions from the root cell */
		mem = jailhouse_cell_mem_regions(cell->config);
		for (n = 0; n < cell->config->num_memory_regions; n++, mem++)
			if (mem->flags & JAILHOUSE_MEM_LOADABLE) {
				err = unmap_from_root_cell(mem);
				if (err)
					goto out_resume;
			}

		arch_config_commit(cpu_data, NULL);

		cell->loadable = false;
	}

	/* present a consistent Communication Region state to the cell */
	cell->comm_page.comm_region.cell_state = JAILHOUSE_CELL_RUNNING;
	cell->comm_page.comm_region.msg_to_cell = JAILHOUSE_MSG_NONE;

	for_each_cpu(cpu, cell->cpu_set) {
		per_cpu(cpu)->failed = false;
		arch_reset_cpu(cpu);
	}

	printk("Started cell \"%s\"\n", cell->config->name);

out_resume:
	cell_resume(cpu_data);

	return err;
}

static int cell_set_loadable(struct per_cpu *cpu_data, unsigned long id)
{
	const struct jailhouse_memory *mem;
	unsigned int cpu, n;
	struct cell *cell;
	int err;

	err = cell_management_prologue(CELL_SET_LOADABLE, cpu_data, id, &cell);
	if (err)
		return err;

	for_each_cpu(cpu, cell->cpu_set) {
		per_cpu(cpu)->failed = false;
		arch_park_cpu(cpu);
	}

	if (cell->loadable)
		goto out_resume;

	cell->comm_page.comm_region.cell_state = JAILHOUSE_CELL_SHUT_DOWN;
	cell->loadable = true;

	/* map all loadable memory regions into the root cell */
	mem = jailhouse_cell_mem_regions(cell->config);
	for (n = 0; n < cell->config->num_memory_regions; n++, mem++)
		if (mem->flags & JAILHOUSE_MEM_LOADABLE) {
			err = remap_to_root_cell(mem, ABORT_ON_ERROR);
			if (err)
				goto out_resume;
		}

	arch_config_commit(cpu_data, NULL);

	printk("Cell \"%s\" can be loaded\n", cell->config->name);

out_resume:
	cell_resume(cpu_data);

	return err;
}

static int cell_destroy(struct per_cpu *cpu_data, unsigned long id)
{
	struct cell *cell, *previous;
	int err;

	err = cell_management_prologue(CELL_DESTROY, cpu_data, id, &cell);
	if (err)
		return err;

	printk("Closing cell \"%s\"\n", cell->config->name);

	cell_destroy_internal(cpu_data, cell);

	previous = &root_cell;
	while (previous->next != cell)
		previous = previous->next;
	previous->next = cell->next;
	num_cells--;

	page_free(&mem_pool, cell, cell->data_pages);
	page_map_dump_stats("after cell destruction");

	cell_reconfig_completed();

	cell_resume(cpu_data);

	return 0;
}

static int cell_get_state(struct per_cpu *cpu_data, unsigned long id)
{
	struct cell *cell;

	if (cpu_data->cell != &root_cell)
		return -EPERM;

	/*
	 * We do not need explicit synchronization with cell_create/destroy
	 * because their cell_suspend(root_cell) will not return before we left
	 * this hypercall.
	 */
	for_each_cell(cell)
		if (cell->id == id) {
			u32 state = cell->comm_page.comm_region.cell_state;

			switch (state) {
			case JAILHOUSE_CELL_RUNNING:
			case JAILHOUSE_CELL_RUNNING_LOCKED:
			case JAILHOUSE_CELL_SHUT_DOWN:
			case JAILHOUSE_CELL_FAILED:
				return state;
			default:
				return -EINVAL;
			}
		}
	return -ENOENT;
}

static int shutdown(struct per_cpu *cpu_data)
{
	unsigned int this_cpu = cpu_data->cpu_id;
	struct cell *cell;
	unsigned int cpu;
	int state, ret;

	/* We do not support shutdown over non-root cells. */
	if (cpu_data->cell != &root_cell)
		return -EPERM;

	spin_lock(&shutdown_lock);

	if (cpu_data->shutdown_state == SHUTDOWN_NONE) {
		state = SHUTDOWN_STARTED;
		for_each_non_root_cell(cell)
			if (!cell_shutdown_ok(cell))
				state = -EPERM;

		if (state == SHUTDOWN_STARTED) {
			printk("Shutting down hypervisor\n");

			for_each_non_root_cell(cell) {
				cell_suspend(cell, cpu_data);

				printk("Closing cell \"%s\"\n",
				       cell->config->name);

				for_each_cpu(cpu, cell->cpu_set) {
					printk(" Releasing CPU %d\n", cpu);
					arch_shutdown_cpu(cpu);
				}
			}

			printk("Closing root cell \"%s\"\n",
			       root_cell.config->name);
			arch_shutdown();
		}

		for_each_cpu(cpu, root_cell.cpu_set)
			per_cpu(cpu)->shutdown_state = state;
	}

	if (cpu_data->shutdown_state == SHUTDOWN_STARTED) {
		printk(" Releasing CPU %d\n", this_cpu);
		ret = 0;
	} else
		ret = cpu_data->shutdown_state;
	cpu_data->shutdown_state = SHUTDOWN_NONE;

	spin_unlock(&shutdown_lock);

	return ret;
}

static long hypervisor_get_info(struct per_cpu *cpu_data, unsigned long type)
{
	switch (type) {
	case JAILHOUSE_INFO_MEM_POOL_SIZE:
		return mem_pool.pages;
	case JAILHOUSE_INFO_MEM_POOL_USED:
		return mem_pool.used_pages;
	case JAILHOUSE_INFO_REMAP_POOL_SIZE:
		return remap_pool.pages;
	case JAILHOUSE_INFO_REMAP_POOL_USED:
		return remap_pool.used_pages;
	case JAILHOUSE_INFO_NUM_CELLS:
		return num_cells;
	default:
		return -EINVAL;
	}
}

static int cpu_get_info(struct per_cpu *cpu_data, unsigned long cpu_id,
			unsigned long type)
{
	if (!cpu_id_valid(cpu_id))
		return -EINVAL;

	/*
	 * We do not need explicit synchronization with cell_destroy because
	 * its cell_suspend(root_cell + this_cell) will not return before we
	 * left this hypercall.
	 */
	if (cpu_data->cell != &root_cell &&
	    !cell_owns_cpu(cpu_data->cell, cpu_id))
		return -EPERM;

	if (type == JAILHOUSE_CPU_INFO_STATE) {
		return per_cpu(cpu_id)->failed ? JAILHOUSE_CPU_FAILED :
			JAILHOUSE_CPU_RUNNING;
	} else if (type >= JAILHOUSE_CPU_INFO_STAT_BASE &&
		type - JAILHOUSE_CPU_INFO_STAT_BASE < JAILHOUSE_NUM_CPU_STATS) {
		type -= JAILHOUSE_CPU_INFO_STAT_BASE;
		return per_cpu(cpu_id)->stats[type] & BIT_MASK(30, 0);
	} else
		return -EINVAL;
}

long hypercall(struct per_cpu *cpu_data, unsigned long code,
	       unsigned long arg1, unsigned long arg2)
{
	cpu_data->stats[JAILHOUSE_CPU_STAT_VMEXITS_HYPERCALL]++;

	switch (code) {
	case JAILHOUSE_HC_DISABLE:
		return shutdown(cpu_data);
	case JAILHOUSE_HC_CELL_CREATE:
		return cell_create(cpu_data, arg1);
	case JAILHOUSE_HC_CELL_START:
		return cell_start(cpu_data, arg1);
	case JAILHOUSE_HC_CELL_SET_LOADABLE:
		return cell_set_loadable(cpu_data, arg1);
	case JAILHOUSE_HC_CELL_DESTROY:
		return cell_destroy(cpu_data, arg1);
	case JAILHOUSE_HC_HYPERVISOR_GET_INFO:
		return hypervisor_get_info(cpu_data, arg1);
	case JAILHOUSE_HC_CELL_GET_STATE:
		return cell_get_state(cpu_data, arg1);
	case JAILHOUSE_HC_CPU_GET_INFO:
		return cpu_get_info(cpu_data, arg1, arg2);
	default:
		return -ENOSYS;
	}
}

void panic_stop(struct per_cpu *cpu_data)
{
	panic_printk("Stopping CPU");
	if (cpu_data) {
		panic_printk(" %d", cpu_data->cpu_id);
		cpu_data->cpu_stopped = true;
	}
	panic_printk("\n");

	if (phys_processor_id() == panic_cpu)
		panic_in_progress = 0;

	arch_panic_stop(cpu_data);
}

void panic_halt(struct per_cpu *cpu_data)
{
	struct cell *cell = cpu_data->cell;
	bool cell_failed = true;
	unsigned int cpu;

	panic_printk("Parking CPU %d\n", cpu_data->cpu_id);

	cpu_data->failed = true;
	for_each_cpu(cpu, cell->cpu_set)
		if (!per_cpu(cpu)->failed) {
			cell_failed = false;
			break;
		}
	if (cell_failed)
		cell->comm_page.comm_region.cell_state = JAILHOUSE_CELL_FAILED;

	arch_panic_halt(cpu_data);

	if (phys_processor_id() == panic_cpu)
		panic_in_progress = 0;
}
