/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fl2000_gma.c
 *
 * Based on GEM CMA allocator helper functions and Xen para-virtual DRM device
 * driver.
 *
 * (C) Copyright 2011 Samsung Electronics Co., Ltd.
 * (C) Copyright 2012 Sascha Hauer, Pengutronix
 * (C) Copyright 2016-2018 EPAM Systems Inc.
 * (C) Copyright 2020, Artem Mygaiev
 */

#include "fl2000.h"

struct fl2000_gem_object {
        struct drm_gem_object base;
        size_t num_pages;
        struct page **pages;
        struct sg_table *sgt;
};

#define to_fl2000_gem_obj(gem_obj) \
	container_of(gem_obj, struct fl2000_gem_object, base)

static struct fl2000_gem_object *
fl2000_gem_allocate_object(struct drm_device *drm, size_t size)
{
	struct fl2000_gem_object *obj;
	struct drm_gem_object *gem_obj;
	int ret;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	gem_obj = &obj->base;

	ret = drm_gem_object_init(drm, gem_obj, size);
	if (ret)
		goto error;

	ret = drm_gem_create_mmap_offset(gem_obj);
	if (ret) {
		drm_gem_object_release(gem_obj);
		goto error;
	}

	return obj;

error:
	kfree(obj);
	return ERR_PTR(ret);
}

static struct fl2000_gem_object *fl2000_gem_create(struct drm_device *drm,
		size_t size)
{
	struct fl2000_gem_object *obj;
	int ret;

	size = round_up(size, PAGE_SIZE);

	obj = fl2000_gem_allocate_object(drm, size);
	if (IS_ERR(obj))
		return obj;

	obj->num_pages = DIV_ROUND_UP(size, PAGE_SIZE);
	obj->pages = drm_gem_get_pages(&obj->base);
	if (IS_ERR(obj->pages)) {
		ret = PTR_ERR(obj->pages);
		goto error;
	}

	return obj;

error:
	drm_gem_object_put_unlocked(&obj->base);
	return ERR_PTR(ret);
}

static struct fl2000_gem_object *
fl2000_gem_create_with_handle(struct drm_file *file_priv,
		struct drm_device *drm, size_t size, uint32_t *handle)
{
	struct fl2000_gem_object *obj;
	struct drm_gem_object *gem_obj;
	int ret;

	obj = fl2000_gem_create(drm, size);
	if (IS_ERR(obj))
		return obj;

	gem_obj = &obj->base;

	/*
	 * allocate a id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file_priv, gem_obj, handle);

	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put_unlocked(gem_obj);

	if (ret)
		return ERR_PTR(ret);

	return obj;
}

void fl2000_gem_free(struct drm_gem_object *gem_obj)
{
	struct fl2000_gem_object *obj;

	obj = to_fl2000_gem_obj(gem_obj);

	if (gem_obj->import_attach) {
		drm_prime_gem_destroy(gem_obj, obj->sgt);
		kvfree(obj->pages);
	} else if (obj->pages) {
		/* TODO: check if flags are properly set */
		drm_gem_put_pages(gem_obj, obj->pages, true, false);
	}

	drm_gem_object_release(gem_obj);

	kfree(obj);
}

int fl2000_gem_dumb_create(struct drm_file *file_priv, struct drm_device *drm,
		struct drm_mode_create_dumb *args)
{
	struct fl2000_gem_object *obj;

	args->pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	args->size = args->pitch * args->height;

	obj = fl2000_gem_create_with_handle(file_priv, drm, args->size,
			&args->handle);
	return PTR_ERR_OR_ZERO(obj);
}

const struct vm_operations_struct fl2000_gem_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static int fl2000_gem_mmap_obj(struct fl2000_gem_object *obj,
		struct vm_area_struct *vma)
{
	int ret;

	/*
	 * Clear the VM_PFNMAP flag that was set by drm_gem_mmap(), and set the
	 * vm_pgoff (used as a fake buffer offset by DRM) to 0 as we want to map
	 * the whole buffer.
	 */
	vma->vm_flags &= ~VM_PFNMAP;
	vma->vm_pgoff = 0;

	ret = vm_map_pages(vma, obj->pages, obj->num_pages);
	if (ret)
		drm_gem_vm_close(vma);

	return ret;
}

int fl2000_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct fl2000_gem_object *obj;
	struct drm_gem_object *gem_obj;
	int ret;

	ret = drm_gem_mmap(filp, vma);
	if (ret)
		return ret;

	gem_obj = vma->vm_private_data;
	obj = to_fl2000_gem_obj(gem_obj);

	return fl2000_gem_mmap_obj(obj, vma);
}

static void fl2000_gem_print_info(struct drm_printer *p, unsigned int indent,
		const struct drm_gem_object *gem_obj)
{
	const struct fl2000_gem_object *obj = to_fl2000_gem_obj(gem_obj);
	drm_printf_indent(p, indent, "num_pages=%zu\n", obj->num_pages);
}

struct sg_table *fl2000_gem_prime_get_sg_table(struct drm_gem_object *gem_obj)
{
	struct fl2000_gem_object *obj = to_fl2000_gem_obj(gem_obj);

	if (!obj->pages)
		return ERR_PTR(-ENOMEM);

	return drm_prime_pages_to_sg(obj->pages, obj->num_pages);
}

struct drm_gem_object *fl2000_gem_prime_import_sg_table(struct drm_device *dev,
		struct dma_buf_attachment *attach, struct sg_table *sgt)
{
	int ret;
	struct fl2000_gem_object *obj;
	size_t size = attach->dmabuf->size;

	obj = fl2000_gem_allocate_object(dev, size);
	if (IS_ERR(obj))
		return ERR_CAST(obj);

	obj->sgt = sgt;
	obj->num_pages = DIV_ROUND_UP(size, PAGE_SIZE);
	obj->pages = kvmalloc_array(obj->num_pages, sizeof(struct page *),
			GFP_KERNEL);
	if (!obj->pages)
		return ERR_PTR(-ENOMEM);

	ret = drm_prime_sg_to_page_addr_arrays(sgt, obj->pages, NULL,
			obj->num_pages);
	if (ret < 0)
		return ERR_PTR(ret);

	return &obj->base;
}

void *fl2000_gem_prime_vmap(struct drm_gem_object *gem_obj)
{
	struct fl2000_gem_object *obj = to_fl2000_gem_obj(gem_obj);

	return vmap(obj->pages, obj->num_pages, VM_MAP, PAGE_KERNEL);
}

void fl2000_gem_prime_vunmap(struct drm_gem_object *gem_obj, void *vaddr)
{
	vunmap(vaddr);
}

static const struct drm_gem_object_funcs fl2000_gem_default_funcs = {
	.free = fl2000_gem_free,
	.print_info = fl2000_gem_print_info,
	.get_sg_table = fl2000_gem_prime_get_sg_table,
	.vmap = fl2000_gem_prime_vmap,
	.vm_ops = &fl2000_gem_vm_ops,
};

/**
 * fl2000_gem_create_object_default_funcs - Create a FL2K GEM object with a
 *                                           default function table
 * @dev: DRM device
 * @size: Size of the object to allocate
 *
 * This sets the GEM object functions to the default CMA helper functions.
 * This function can be used as the &drm_driver.gem_create_object callback.
 *
 * Returns:
 * A pointer to a allocated GEM object or an error pointer on failure.
 */
struct drm_gem_object *
fl2000_gem_create_object_default_funcs(struct drm_device *dev, size_t size)
{
	struct fl2000_gem_object *obj;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return NULL;

	obj->base.funcs = &fl2000_gem_default_funcs;

	return &obj->base;
}

void *fl2000_gem_get_fb_addr(struct drm_framebuffer *fb,
		struct drm_plane_state *state, unsigned int plane)
{
	struct fl2000_gem_object *obj;
	struct drm_gem_object *gem_obj;
	void *vaddr;
	u8 h_div = 1, v_div = 1;
	u32 block_w = drm_format_info_block_width(fb->format, plane);
	u32 block_h = drm_format_info_block_height(fb->format, plane);
	u32 block_size = fb->format->char_per_block[plane];
	u32 sample_x;
	u32 sample_y;
	u32 block_start_y;
	u32 num_hblocks;

	gem_obj = drm_gem_fb_get_obj(fb, plane);
	if (!gem_obj) {
		return NULL;
	}

	obj = to_fl2000_gem_obj(gem_obj);

	vaddr = vmap(obj->pages, obj->num_pages, VM_MAP, PAGE_KERNEL);
	if (!vaddr) {
		return NULL;
	}

	vaddr += fb->offsets[plane];

	if (plane > 0) {
		h_div = fb->format->hsub;
		v_div = fb->format->vsub;
	}

	sample_x = (state->src_x >> 16) / h_div;
	sample_y = (state->src_y >> 16) / v_div;
	block_start_y = (sample_y / block_h) * block_h;
	num_hblocks = sample_x / block_w;

	vaddr += fb->pitches[plane] * block_start_y;
	vaddr += block_size * num_hblocks;

	return vaddr;
}
