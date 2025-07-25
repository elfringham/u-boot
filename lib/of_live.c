// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2009 Benjamin Herrenschmidt, IBM Corp
 * benh@kernel.crashing.org
 *
 * Based on parts of drivers/of/fdt.c from Linux v4.9
 * Modifications for U-Boot
 * Copyright (c) 2017 Google, Inc
 */

#define LOG_CATEGORY	LOGC_DT

#include <abuf.h>
#include <event.h>
#include <log.h>
#include <linux/libfdt.h>
#include <of_live.h>
#include <malloc.h>
#include <dm/of_access.h>
#include <linux/err.h>
#include <linux/sizes.h>

enum {
	BUF_STEP	= SZ_64K,
};

static void *unflatten_dt_alloc(void **mem, unsigned long size,
				unsigned long align)
{
	void *res;

	*mem = PTR_ALIGN(*mem, align);
	res = *mem;
	*mem += size;

	return res;
}

/**
 * unflatten_dt_node() - Alloc and populate a device_node from the flat tree
 * @blob: The parent device tree blob
 * @mem: Memory chunk to use for allocating device nodes and properties
 * @poffset: pointer to node in flat tree
 * @dad: Parent struct device_node
 * @nodepp: The device_node tree created by the call
 * @fpsize: Size of the node path up at t05he current depth.
 * @dryrun: If true, do not allocate device nodes but still calculate needed
 * memory size
 */
static void *unflatten_dt_node(const void *blob, void *mem, int *poffset,
			       struct device_node *dad,
			       struct device_node **nodepp,
			       unsigned long fpsize, bool dryrun)
{
	const __be32 *p;
	struct device_node *np;
	struct property *pp, **prev_pp = NULL;
	const char *pathp;
	int l;
	unsigned int allocl;
	static int depth;
	int old_depth;
	int offset;
	int has_name = 0;
	int new_format = 0;

	pathp = fdt_get_name(blob, *poffset, &l);
	if (!pathp)
		return mem;

	allocl = ++l;

	/*
	 * version 0x10 has a more compact unit name here instead of the full
	 * path. we accumulate the full path size using "fpsize", we'll rebuild
	 * it later. We detect this because the first character of the name is
	 * not '/'.
	 */
	if ((*pathp) != '/') {
		new_format = 1;
		if (fpsize == 0) {
			/*
			 * root node: special case. fpsize accounts for path
			 * plus terminating zero. root node only has '/', so
			 * fpsize should be 2, but we want to avoid the first
			 * level nodes to have two '/' so we use fpsize 1 here
			 */
			fpsize = 1;
			allocl = 2;
			l = 1;
			pathp = "";
		} else {
			/*
			 * account for '/' and path size minus terminal 0
			 * already in 'l'
			 */
			fpsize += l;
			allocl = fpsize;
		}
	}

	np = unflatten_dt_alloc(&mem, sizeof(struct device_node) + allocl,
				__alignof__(struct device_node));
	if (!dryrun) {
		char *fn;

		fn = (char *)np + sizeof(*np);
		if (new_format) {
			np->name = pathp;
			has_name = 1;
		}
		np->full_name = fn;
		if (new_format) {
			/* rebuild full path for new format */
			if (dad && dad->parent) {
				strcpy(fn, dad->full_name);
#ifdef DEBUG
				if ((strlen(fn) + l + 1) != allocl) {
					debug("%s: p: %d, l: %d, a: %d\n",
					      pathp, (int)strlen(fn), l,
					      allocl);
				}
#endif
				fn += strlen(fn);
			}
			*(fn++) = '/';
		}
		memcpy(fn, pathp, l);

		prev_pp = &np->properties;
		if (dad != NULL) {
			np->parent = dad;
			np->sibling = dad->child;
			dad->child = np;
		}
	}
	/* process properties */
	for (offset = fdt_first_property_offset(blob, *poffset);
	     (offset >= 0);
	     (offset = fdt_next_property_offset(blob, offset))) {
		const char *pname;
		int sz;

		p = fdt_getprop_by_offset(blob, offset, &pname, &sz);
		if (!p) {
			offset = -FDT_ERR_INTERNAL;
			break;
		}

		if (pname == NULL) {
			debug("Can't find property name in list !\n");
			break;
		}
		if (strcmp(pname, "name") == 0)
			has_name = 1;
		pp = unflatten_dt_alloc(&mem, sizeof(struct property),
					__alignof__(struct property));
		if (!dryrun) {
			/*
			 * We accept flattened tree phandles either in
			 * ePAPR-style "phandle" properties, or the
			 * legacy "linux,phandle" properties.  If both
			 * appear and have different values, things
			 * will get weird.  Don't do that. */
			if ((strcmp(pname, "phandle") == 0) ||
			    (strcmp(pname, "linux,phandle") == 0)) {
				if (np->phandle == 0)
					np->phandle = be32_to_cpup(p);
			}
			/*
			 * And we process the "ibm,phandle" property
			 * used in pSeries dynamic device tree
			 * stuff */
			if (strcmp(pname, "ibm,phandle") == 0)
				np->phandle = be32_to_cpup(p);
			pp->name = (char *)pname;
			pp->length = sz;
			pp->value = (__be32 *)p;
			*prev_pp = pp;
			prev_pp = &pp->next;
		}
	}
	/*
	 * with version 0x10 we may not have the name property, recreate
	 * it here from the unit name if absent
	 */
	if (!has_name) {
		const char *p1 = pathp, *ps = pathp, *pa = NULL;
		int sz;

		while (*p1) {
			if ((*p1) == '@')
				pa = p1;
			if ((*p1) == '/')
				ps = p1 + 1;
			p1++;
		}
		if (pa < ps)
			pa = p1;
		sz = (pa - ps) + 1;
		pp = unflatten_dt_alloc(&mem, sizeof(struct property) + sz,
					__alignof__(struct property));
		if (!dryrun) {
			pp->name = "name";
			pp->length = sz;
			pp->value = pp + 1;
			*prev_pp = pp;
			prev_pp = &pp->next;
			memcpy(pp->value, ps, sz - 1);
			((char *)pp->value)[sz - 1] = 0;
			debug("fixed up name for %s -> %s\n", pathp,
			      (char *)pp->value);
		}
	}
	if (!dryrun) {
		*prev_pp = NULL;
		if (!has_name)
			np->name = of_get_property(np, "name", NULL);
		np->type = of_get_property(np, "device_type", NULL);

		if (!np->name)
			np->name = "<NULL>";
		if (!np->type)
			np->type = "<NULL>";	}

	old_depth = depth;
	*poffset = fdt_next_node(blob, *poffset, &depth);
	if (depth < 0)
		depth = 0;
	while (*poffset > 0 && depth > old_depth) {
		mem = unflatten_dt_node(blob, mem, poffset, np, NULL,
					fpsize, dryrun);
		if (!mem)
			return NULL;
	}

	if (*poffset < 0 && *poffset != -FDT_ERR_NOTFOUND) {
		debug("unflatten: error %d processing FDT\n", *poffset);
		return NULL;
	}

	/*
	 * Reverse the child list. Some drivers assumes node order matches .dts
	 * node order
	 */
	if (!dryrun && np->child) {
		struct device_node *child = np->child;
		np->child = NULL;
		while (child) {
			struct device_node *next = child->sibling;

			child->sibling = np->child;
			np->child = child;
			child = next;
		}
	}

	if (nodepp)
		*nodepp = np;

	return mem;
}

int unflatten_device_tree(const void *blob, struct device_node **mynodes)
{
	unsigned long size;
	int start;
	void *mem;

	debug(" -> unflatten_device_tree()\n");

	if (!blob) {
		debug("No device tree pointer\n");
		return -EINVAL;
	}

	debug("Unflattening device tree:\n");
	debug("magic: %08x\n", fdt_magic(blob));
	debug("size: %08x\n", fdt_totalsize(blob));
	debug("version: %08x\n", fdt_version(blob));

	if (fdt_check_header(blob)) {
		debug("Invalid device tree blob header\n");
		return -EINVAL;
	}

	/* First pass, scan for size */
	start = 0;
	size = (unsigned long)unflatten_dt_node(blob, NULL, &start, NULL, NULL,
						0, true);
	if (!size)
		return -EFAULT;
	size = ALIGN(size, 4);

	debug("  size is %lx, allocating...\n", size);

	/* Allocate memory for the expanded device tree */
	mem = memalign(__alignof__(struct device_node), size + 4);
	memset(mem, '\0', size);

	/* Set up value for dm_test_livetree_align() */
	*(u32 *)mem = BAD_OF_ROOT;

	*(__be32 *)(mem + size) = cpu_to_be32(0xdeadbeef);

	debug("  unflattening %p...\n", mem);

	/* Second pass, do actual unflattening */
	start = 0;
	unflatten_dt_node(blob, mem, &start, NULL, mynodes, 0, false);
	if (be32_to_cpup(mem + size) != 0xdeadbeef) {
		debug("End of tree marker overwritten: %08x\n",
		      be32_to_cpup(mem + size));
		return -ENOSPC;
	}

	debug(" <- unflatten_device_tree()\n");

	return 0;
}

int of_live_build(const void *fdt_blob, struct device_node **rootp)
{
	int ret;
	union event_data evt;

	debug("%s: start\n", __func__);
	ret = unflatten_device_tree(fdt_blob, rootp);
	if (ret) {
		debug("Failed to create live tree: err=%d\n", ret);
		return ret;
	}
	ret = of_alias_scan();
	if (ret) {
		debug("Failed to scan live tree aliases: err=%d\n", ret);
		return ret;
	}
	debug("%s: stop\n", __func__);

	if (CONFIG_IS_ENABLED(EVENT)) {
		evt.of_live_built.root = *rootp;
		ret = event_notify(EVT_OF_LIVE_BUILT, &evt, sizeof(evt));
		if (ret) {
			log_debug("Failed to notify livetree build event: err=%d\n", ret);
			return ret;
		}
	}

	return ret;
}

void of_live_free(struct device_node *root)
{
	/* the tree is stored as a contiguous block of memory */
	free(root);
}

int of_live_create_empty(struct device_node **rootp)
{
	struct device_node *root;

	root = calloc(1, sizeof(struct device_node));
	if (!root)
		return -ENOMEM;
	root->name = strdup("");
	if (!root->name) {
		free(root);
		return -ENOMEM;
	}
	root->type = "<NULL>";
	root->full_name = "";
	*rootp = root;

	return 0;
}

static int check_space(int ret, struct abuf *buf)
{
	if (ret == -FDT_ERR_NOSPACE) {
		if (!abuf_realloc_inc(buf, BUF_STEP))
			return log_msg_ret("spc", -ENOMEM);
		ret = fdt_resize(abuf_data(buf), abuf_data(buf),
				 abuf_size(buf));
		if (ret)
			return log_msg_ret("res", -EFAULT);

		return -EAGAIN;
	}

	return 0;
}

/**
 * flatten_node() - Write out the node and its properties into a flat tree
 */
static int flatten_node(struct abuf *buf, const struct device_node *node)
{
	const struct device_node *np;
	const struct property *pp;
	int ret;

	ret = fdt_begin_node(abuf_data(buf), node->name);
	ret = check_space(ret, buf);
	if (ret == -EAGAIN) {
		ret = fdt_begin_node(abuf_data(buf), node->name);
		if (ret) {
			log_debug("Internal error a %d\n", ret);
			return -EFAULT;
		}
	}
	if (ret)
		return log_msg_ret("beg", ret);

	/* First write out the properties */
	for (pp = node->properties; !ret && pp; pp = pp->next) {
		ret = fdt_property(abuf_data(buf), pp->name, pp->value,
				   pp->length);
		ret = check_space(ret, buf);
		if (ret == -EAGAIN) {
			ret = fdt_property(abuf_data(buf), pp->name, pp->value,
					   pp->length);
		}
	}

	/* Next write out the subnodes */
	for (np = node->child; np; np = np->sibling) {
		ret = flatten_node(buf, np);
		if (ret)
			return log_msg_ret("sub", ret);
	}

	ret = fdt_end_node(abuf_data(buf));
	ret = check_space(ret, buf);
	if (ret == -EAGAIN) {
		ret = fdt_end_node(abuf_data(buf));
		if (ret) {
			log_debug("Internal error b %d\n", ret);
			return -EFAULT;
		}
	}
	if (ret)
		return log_msg_ret("end", ret);

	return 0;
}

int of_live_flatten(const struct device_node *root, struct abuf *buf)
{
	int ret;

	if (!abuf_init_size(buf, BUF_STEP))
		return log_msg_ret("ini", -ENOMEM);

	ret = fdt_create(abuf_data(buf), abuf_size(buf));
	if (!ret)
		ret = fdt_finish_reservemap(abuf_data(buf));
	if (ret) {
		log_debug("Failed to start FDT (err=%d)\n", ret);
		return log_msg_ret("sta", -EINVAL);
	}

	ret = flatten_node(buf, root);
	if (ret)
		return log_msg_ret("flt", ret);

	ret = fdt_finish(abuf_data(buf));
	ret = check_space(ret, buf);
	if (ret == -EAGAIN) {
		ret = fdt_finish(abuf_data(buf));
		if (ret) {
			log_debug("Internal error c %d\n", ret);
			return -EFAULT;
		}
	}
	if (ret)
		return log_msg_ret("fin", ret);

	ret = fdt_pack(abuf_data(buf));
	if (ret) {
		log_debug("Failed to pack (err=%d)\n", ret);
		return log_msg_ret("pac", -EFAULT);
	}

	if (!abuf_realloc(buf, fdt_totalsize(abuf_data(buf))))
		return log_msg_ret("abu", -EFAULT);

	return 0;
}
