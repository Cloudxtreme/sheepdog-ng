/*
 * Copyright (C) 2009-2011 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "sheep_priv.h"

struct vdi_state_entry {
	uint32_t vid;
	bool snapshot;
	struct rb_node node;
};

static struct rb_root vdi_state_root = RB_ROOT;
static struct sd_rw_lock vdi_state_lock = SD_RW_LOCK_INITIALIZER;

/*
 * ec_max_data_strip represent max number of data strips in the cluster. When
 * nr_zones < it, we don't purge the stale objects because for erasure coding,
 * there is only one copy of data.
 */
int ec_max_data_strip(void)
{
	int d;

	if (!sys->cinfo.copy_policy)
		return 0;

	ec_policy_to_dp(sys->cinfo.copy_policy, &d, NULL);

	return d;
}

int sheep_bnode_writer(uint64_t oid, void *mem, unsigned int len,
		       uint64_t offset, uint32_t flags, int copies,
		       int copy_policy, bool create, bool direct)
{
	return sd_write_object(oid, mem, len, offset, create);
}

int sheep_bnode_reader(uint64_t oid, void **mem, unsigned int len,
		       uint64_t offset)
{
	return sd_read_object(oid, *mem, len, offset);
}

static int vdi_state_cmp(const struct vdi_state_entry *a,
			 const struct vdi_state_entry *b)
{
	return intcmp(a->vid, b->vid);
}

static struct vdi_state_entry *vdi_state_search(struct rb_root *root,
						uint32_t vid)
{
	struct vdi_state_entry key = { .vid = vid };

	return rb_search(root, &key, node, vdi_state_cmp);
}

static struct vdi_state_entry *vdi_state_insert(struct rb_root *root,
						struct vdi_state_entry *new)
{
	return rb_insert(root, new, node, vdi_state_cmp);
}

static bool vid_is_snapshot(uint32_t vid)
{
	struct vdi_state_entry *entry;

	sd_read_lock(&vdi_state_lock);
	entry = vdi_state_search(&vdi_state_root, vid);
	sd_rw_unlock(&vdi_state_lock);

	if (!entry) {
		sd_debug("No VDI entry for %" PRIx32 " found", vid);
		return false;
	}

	return entry->snapshot;
}

bool oid_is_readonly(uint64_t oid)
{
	/* we allow changing snapshot attributes */
	if (!is_data_obj(oid))
		return false;

	return vid_is_snapshot(oid_to_vid(oid));
}

int get_vdi_copy_number(uint32_t vid)
{
	return sys->cinfo.nr_copies;
}

int get_vdi_copy_policy(uint32_t vid)
{
	return sys->cinfo.copy_policy;
}

int get_obj_copy_number(uint64_t oid, int nr_zones)
{
	return min(get_vdi_copy_number(oid_to_vid(oid)), nr_zones);
}

int get_req_copy_number(struct request *req)
{
	int nr_copies;

	nr_copies = min((int)req->rq.obj.copies, req->vinfo->nr_zones);
	if (!nr_copies)
		nr_copies = get_obj_copy_number(req->rq.obj.oid,
						req->vinfo->nr_zones);

	return nr_copies;
}

void vdi_mark_snapshot(uint32_t vid)
{
	struct vdi_state_entry *entry, *old;

	entry = xzalloc(sizeof(*entry));
	entry->vid = vid;
	entry->snapshot = true;

	sd_debug("%" PRIx32, vid);

	sd_write_lock(&vdi_state_lock);
	old = vdi_state_insert(&vdi_state_root, entry);
	if (old) {
		free(entry);
		entry = old;
		entry->snapshot = true;
	}

	sd_rw_unlock(&vdi_state_lock);
}

void vdi_delete_state(uint32_t vid)
{
	struct vdi_state_entry *entry;

	sd_debug("%"PRIx32, vid);
	sd_read_lock(&vdi_state_lock);
	entry = vdi_state_search(&vdi_state_root, vid);
	if (entry)
		rb_erase(&entry->node, &vdi_state_root);
	sd_rw_unlock(&vdi_state_lock);
}

static inline bool vdi_is_deleted(struct sd_inode *inode)
{
	return *inode->name == '\0';
}

int vdi_exist(uint32_t vid)
{
	struct sd_inode *inode;
	int ret;

	inode = xzalloc(sizeof(*inode));
	ret = sd_read_object(vid_to_vdi_oid(vid), (char *)inode,
			     sizeof(*inode), 0);
	if (ret != SD_RES_SUCCESS) {
		sd_err("fail to read vdi inode (%" PRIx32 ")", vid);
		ret = 0;
		goto out;
	}

	if (vdi_is_deleted(inode)) {
		ret = 0;
		goto out;
	}
	ret = 1;
out:
	free(inode);
	return ret;
}

static struct sd_inode *alloc_inode(const struct vdi_iocb *iocb,
				    uint32_t new_snapid, uint32_t new_vid,
				    uint32_t *data_vdi_id,
				    struct generation_reference *gref)
{
	struct sd_inode *new = xzalloc(sizeof(*new));

	pstrcpy(new->name, sizeof(new->name), iocb->name);
	new->vdi_id = new_vid;
	new->create_time = iocb->time;
	new->vdi_size = iocb->size;
	new->copy_policy = iocb->copy_policy;
	new->store_policy = iocb->store_policy;
	new->nr_copies = iocb->nr_copies;
	new->block_size_shift = SD_DEFAULT_BLOCK_SIZE_SHIFT;
	new->snap_id = new_snapid;
	new->parent_vdi_id = iocb->base_vid;
	if (data_vdi_id)
		sd_inode_copy_vdis(sheep_bnode_writer, sheep_bnode_reader,
				   data_vdi_id, iocb->store_policy,
				   iocb->nr_copies, iocb->copy_policy, new);
	else if (new->store_policy)
		sd_inode_init(new->data_vdi_id, 1);

	if (gref) {
		sd_assert(data_vdi_id);

		for (int i = 0; i < SD_INODE_DATA_INDEX; i++) {
			if (!data_vdi_id[i])
				continue;

			new->gref[i].generation = gref[i].generation + 1;
		}
	}

	return new;
}

/* Create a fresh vdi */
static int create_vdi(const struct vdi_iocb *iocb, uint32_t new_snapid,
		      uint32_t new_vid)
{
	struct sd_inode *new = alloc_inode(iocb, new_snapid, new_vid, NULL,
					   NULL);
	int ret;

	sd_debug("%s: size %" PRIu64 ", new_vid %" PRIx32 ", copies %d, "
		 "snapid %" PRIu32 " copy policy %"PRIu8 "store policy %"PRIu8,
		 iocb->name, iocb->size, new_vid, iocb->nr_copies, new_snapid,
		 new->copy_policy, new->store_policy);

	ret = sd_write_object(vid_to_vdi_oid(new_vid), (char *)new,
			      sizeof(*new), 0, true);
	if (ret != SD_RES_SUCCESS)
		ret = SD_RES_VDI_WRITE;

	free(new);
	return ret;
}

/*
 * Create a clone vdi from the existing snapshot
 *
 * This creates a working vdi 'new' based on the snapshot 'base'.  For example:
 *
 * [before]
 *                base
 *            o----o----o----x
 *
 * [after]
 *                base
 *            o----o----o----x
 *                  \
 *                   x new
 * x: working vdi
 * o: snapshot vdi
 */
static int clone_vdi(const struct vdi_iocb *iocb, uint32_t new_snapid,
		     uint32_t new_vid, uint32_t base_vid)
{
	struct sd_inode *new = NULL, *base = xzalloc(sizeof(*base));
	int ret;

	sd_debug("%s: size %" PRIu64 ", vid %" PRIx32 ", base %" PRIx32 ", "
		 "copies %d, snapid %" PRIu32, iocb->name, iocb->size, new_vid,
		 base_vid, iocb->nr_copies, new_snapid);

	ret = sd_read_object(vid_to_vdi_oid(base_vid), (char *)base,
			     sizeof(*base), 0);
	if (ret != SD_RES_SUCCESS) {
		ret = SD_RES_BASE_VDI_READ;
		goto out;
	}

	/* TODO: multiple sd_write_object should be performed atomically */

	for (int i = 0; i < ARRAY_SIZE(base->gref); i++) {
		if (base->data_vdi_id[i])
			base->gref[i].count++;
	}

	ret = sd_write_object(vid_to_vdi_oid(base_vid), (char *)base->gref,
			      sizeof(base->gref),
			      offsetof(struct sd_inode, gref), false);
	if (ret != SD_RES_SUCCESS) {
		ret = SD_RES_BASE_VDI_WRITE;
		goto out;
	}

	/* create a new vdi */
	new = alloc_inode(iocb, new_snapid, new_vid, base->data_vdi_id,
			  base->gref);
	ret = sd_write_object(vid_to_vdi_oid(new_vid), (char *)new,
			      sizeof(*new), 0, true);
	if (ret != SD_RES_SUCCESS)
		ret = SD_RES_VDI_WRITE;

out:
	free(new);
	free(base);
	return ret;
}

/*
 * Create a snapshot vdi
 *
 * This makes the current working vdi 'base' a snapshot, and create a working
 * vdi 'new'.  For example:
 *
 * [before]
 *            o----o----o----x base
 *
 * [after]
 *                          base
 *            o----o----o----o----x new
 *
 * x: working vdi
 * o: snapshot vdi
 */
static int snapshot_vdi(const struct vdi_iocb *iocb, uint32_t new_snapid,
			uint32_t new_vid, uint32_t base_vid)
{
	struct sd_inode *new = NULL, *base = xzalloc(sizeof(*base));
	int ret;

	sd_debug("%s: size %" PRIu64 ", vid %" PRIx32 ", base %" PRIx32 ", "
		 "copies %d, snapid %" PRIu32, iocb->name, iocb->size, new_vid,
		 base_vid, iocb->nr_copies, new_snapid);

	ret = sd_read_object(vid_to_vdi_oid(base_vid), (char *)base,
			     sizeof(*base), 0);
	if (ret != SD_RES_SUCCESS) {
		ret = SD_RES_BASE_VDI_READ;
		goto out;
	}

	/* TODO: multiple sd_write_object should be performed atomically */

	/* update a base vdi */
	base->snap_ctime = iocb->time;

	for (int i = 0; i < ARRAY_SIZE(base->gref); i++) {
		if (base->data_vdi_id[i])
			base->gref[i].count++;
	}

	ret = sd_write_object(vid_to_vdi_oid(base_vid), (char *)base,
			      sizeof(*base), 0, false);
	if (ret != SD_RES_SUCCESS) {
		sd_err("updating gref of VDI %" PRIx32 "failed", base_vid);
		ret = SD_RES_BASE_VDI_WRITE;
		goto out;
	}

	/* create a new vdi */
	new = alloc_inode(iocb, new_snapid, new_vid, base->data_vdi_id,
			  base->gref);
	ret = sd_write_object(vid_to_vdi_oid(new_vid), (char *)new,
			      sizeof(*new), 0, true);
	if (ret != SD_RES_SUCCESS)
		ret = SD_RES_VDI_WRITE;

out:
	free(new);
	free(base);
	return ret;
}

/*
 * Rebase onto another snapshot vdi
 *
 * This makes the current working vdi 'base' a snapshot, and create a new
 * working vdi 'new' based on the snapshot 'base'.  We use this operation when
 * rollbacking to the snapshot or writing data to the snapshot.  Here is an
 * example:
 *
 * [before]
 *                base
 *            o----o----o----x cur
 *
 * [after]
 *                base
 *            o----o----o----o cur
 *                  \
 *                   x new
 * x: working vdi
 * o: snapshot vdi
 */
static int rebase_vdi(const struct vdi_iocb *iocb, uint32_t new_snapid,
		      uint32_t new_vid, uint32_t base_vid, uint32_t cur_vid)
{
	struct sd_inode *new = NULL, *base = xzalloc(sizeof(*base));
	int ret;

	sd_debug("%s: size %" PRIu64 ", vid %" PRIx32 ", base %" PRIx32 ", "
		 "cur %" PRIx32 ", copies %d, snapid %" PRIu32, iocb->name,
		 iocb->size, new_vid, base_vid, cur_vid, iocb->nr_copies,
		 new_snapid);

	ret = sd_read_object(vid_to_vdi_oid(base_vid), (char *)base,
			     sizeof(*base), 0);
	if (ret != SD_RES_SUCCESS) {
		ret = SD_RES_BASE_VDI_READ;
		goto out;
	}

	/* TODO: multiple sd_write_object should be performed atomically */

       ret = sd_write_object(vid_to_vdi_oid(cur_vid), (char *)&iocb->time,
                             sizeof(iocb->time),
                             offsetof(struct sd_inode, snap_ctime), false);
	if (ret != SD_RES_SUCCESS) {
		ret = SD_RES_VDI_WRITE;
		goto out;
	}

	for (int i = 0; i < ARRAY_SIZE(base->gref); i++) {
		if (base->data_vdi_id[i])
			base->gref[i].count++;
	}
	/* update current working vdi */
	ret = sd_write_object(vid_to_vdi_oid(base_vid), (char *)base->gref,
			      sizeof(base->gref),
			      offsetof(struct sd_inode, gref), false);
	if (ret != SD_RES_SUCCESS) {
		ret = SD_RES_VDI_WRITE;
		goto out;
	}

	/* create a new vdi */
	new = alloc_inode(iocb, new_snapid, new_vid, base->data_vdi_id,
			  base->gref);
	ret = sd_write_object(vid_to_vdi_oid(new_vid), (char *)new,
			      sizeof(*new), 0, true);
	if (ret != SD_RES_SUCCESS)
		ret = SD_RES_VDI_WRITE;

out:
	free(new);
	free(base);
	return ret;
}

/*
 * Return SUCCESS (range of bits set):
 * Iff we get a bitmap range [left, right) that VDI might be set between. if
 * right < left, this means a wrap around case where we should examine the
 * two split ranges, [left, SD_NR_VDIS - 1] and [0, right). 'Right' is the free
 * bit that might be used by newly created VDI.
 *
 * Otherwise:
 * Return NO_VDI (bit not set) or FULL_VDI (bitmap fully set)
 */
static int get_vdi_bitmap_range(const char *name, unsigned long *left,
				unsigned long *right)
{
	*left = sd_hash_vdi(name);

	if (unlikely(!*left))
		*left = 1;	/* 0x000000 should be skipeed */

	*right = find_next_zero_bit(sys->vdi_inuse, SD_NR_VDIS, *left);
	if (*left == *right)
		return SD_RES_NO_VDI;

	if (*right == SD_NR_VDIS) {
		/* Wrap around */
		*right = find_next_zero_bit(sys->vdi_inuse, SD_NR_VDIS, 1);
		if (*right == SD_NR_VDIS)
			return SD_RES_FULL_VDI;
	}
	return SD_RES_SUCCESS;
}

static inline bool vdi_has_tag(const struct vdi_iocb *iocb)
{
	if ((iocb->tag && iocb->tag[0]) || iocb->snapid)
		return true;
	return false;
}

static inline bool vdi_tag_match(const struct vdi_iocb *iocb,
				 const struct sd_inode *inode)
{
	const char *tag = iocb->tag;

	if (inode->tag[0] && !strncmp(inode->tag, tag, sizeof(inode->tag)))
		return true;
	if (iocb->snapid == inode->snap_id)
		return true;
	return false;
}

static int fill_vdi_info_range(uint32_t left, uint32_t right,
			       const struct vdi_iocb *iocb,
			       struct vdi_info *info)
{
	struct sd_inode *inode;
	bool vdi_found = false;
	int ret;
	uint32_t i;
	const char *name = iocb->name;

	inode = malloc(SD_INODE_HEADER_SIZE);
	if (!inode) {
		sd_err("failed to allocate memory");
		ret = SD_RES_NO_MEM;
		goto out;
	}
	for (i = right - 1; i >= left && i; i--) {
		ret = sd_read_object(vid_to_vdi_oid(i), (char *)inode,
				     SD_INODE_HEADER_SIZE, 0);
		if (ret != SD_RES_SUCCESS)
			goto out;

		if (vdi_is_deleted(inode)) {
			/* Recycle the deleted inode for fresh vdi create */
			if (!iocb->create_snapshot)
				info->free_bit = i;
			continue;
		}

		if (!strncmp(inode->name, name, sizeof(inode->name))) {
			sd_debug("%s = %s, %u = %u", iocb->tag, inode->tag,
				 iocb->snapid, inode->snap_id);
			if (vdi_has_tag(iocb)) {
				/* Read, delete, clone on snapshots */
				if (!vdi_is_snapshot(inode)) {
					vdi_found = true;
					continue;
				}
				if (!vdi_tag_match(iocb, inode))
					continue;
			} else {
				/*
				 * Rollback & snap create, read, delete on
				 * current working VDI
				 */
				info->snapid = inode->snap_id + 1;
				if (vdi_is_snapshot(inode))
					/* Current working VDI is deleted */
					break;
			}
			info->create_time = inode->create_time;
			info->vid = inode->vdi_id;
			goto out;
		}
	}
	ret = vdi_found ? SD_RES_NO_TAG : SD_RES_NO_VDI;
out:
	free(inode);
	return ret;
}

/* Fill the VDI information from right to left in the bitmap */
static int fill_vdi_info(unsigned long left, unsigned long right,
			 const struct vdi_iocb *iocb, struct vdi_info *info)
{
	int ret;

	assert(left != right);
	/*
	 * If left == right, fill_vdi_info() shouldn't called by vdi_lookup().
	 * vdi_lookup() must return SD_RES_NO_VDI to its caller.
	 */

	if (left < right)
		return fill_vdi_info_range(left, right, iocb, info);

	if (likely(1 < right))
		ret = fill_vdi_info_range(1, right, iocb, info);
	else
		ret = SD_RES_NO_VDI;

	switch (ret) {
	case SD_RES_NO_VDI:
	case SD_RES_NO_TAG:
		ret = fill_vdi_info_range(left, SD_NR_VDIS, iocb, info);
		break;
	default:
		break;
	}
	return ret;
}

/* Return SUCCESS if we find targeted VDI specified by iocb and fill info */
int vdi_lookup(const struct vdi_iocb *iocb, struct vdi_info *info)
{
	unsigned long left, right;
	int ret;

	ret = get_vdi_bitmap_range(iocb->name, &left, &right);
	info->free_bit = right;
	sd_debug("%s left %lx right %lx, %x", iocb->name, left, right, ret);
	switch (ret) {
	case SD_RES_NO_VDI:
	case SD_RES_FULL_VDI:
		return ret;
	case SD_RES_SUCCESS:
		break;
	}
	return fill_vdi_info(left, right, iocb, info);
}

static void vdi_flush(uint32_t vid)
{
	struct sd_req hdr;
	int ret;

	sd_init_req(&hdr, SD_OP_FLUSH_VDI);
	hdr.obj.oid = vid_to_vdi_oid(vid);

	ret = exec_local_req(&hdr, NULL);
	if (ret != SD_RES_SUCCESS)
		sd_err("fail to flush vdi %" PRIx32 ", %s", vid,
		       sd_strerror(ret));
}

/*
 * This function creates another working vdi with a new name.  The parent of the
 * newly created vdi is iocb->base_vid.
 *
 * There are 2 vdi create operation in SD:
 * 1. fresh create (base_vid == 0)
 * 2. clone create (base_vid != 0)
 *
 * This function expects NO_VDI returned from vdi_lookup().  Fresh create
 * started with id = 1 when there are no snapshot with the same name.  Working
 * VDI always has the highest snapid.
 */
int vdi_create(const struct vdi_iocb *iocb, uint32_t *new_vid)
{
	struct vdi_info info = {};
	int ret;

	ret = vdi_lookup(iocb, &info);
	switch (ret) {
	case SD_RES_SUCCESS:
		return SD_RES_VDI_EXIST;
	case SD_RES_NO_VDI:
		break;
	default:
		sd_err("%s", sd_strerror(ret));
		return ret;
	}

	if (info.snapid == 0)
		info.snapid = 1;
	*new_vid = info.free_bit;

	if (iocb->base_vid == 0)
		return create_vdi(iocb, info.snapid, *new_vid);
	else
		return clone_vdi(iocb, info.snapid, *new_vid, iocb->base_vid);
}

/*
 * This function makes the current working vdi a snapshot, and create a new
 * working vdi with the same name.  The parent of the newly created vdi is
 * iocb->base_vid.
 *
 * There are 2 snapshot create operation in SD:
 * 1. snapshot create (base_vid == current_vid)
 * 2. rollback create (base_vid != current_vid)
 *
 * This function expects SUCCESS returned from vdi_lookup().  Both rollback and
 * snap create started with current working VDI's snap_id + 1. Working VDI
 * always has the highest snapid.
 */
int vdi_snapshot(const struct vdi_iocb *iocb, uint32_t *new_vid)
{
	struct vdi_info info = {};
	int ret;

	ret = vdi_lookup(iocb, &info);
	if (ret == SD_RES_SUCCESS) {
		if (sys->enable_object_cache)
			vdi_flush(iocb->base_vid);
	} else {
		sd_err("%s", sd_strerror(ret));
		return ret;
	}

	sd_assert(info.snapid > 0);
	*new_vid = info.free_bit;
	if (ret != SD_RES_SUCCESS)
		return ret;

	if (iocb->base_vid == info.vid) {
		return snapshot_vdi(iocb, info.snapid, *new_vid,
				    iocb->base_vid);
	} else
		return rebase_vdi(iocb, info.snapid, *new_vid, iocb->base_vid,
				  info.vid);
}

int read_vdis(char *data, int len, unsigned int *rsp_len)
{
	if (len != sizeof(sys->vdi_inuse))
		return SD_RES_INVALID_PARMS;

	memcpy(data, sys->vdi_inuse, sizeof(sys->vdi_inuse));
	*rsp_len = sizeof(sys->vdi_inuse);

	return SD_RES_SUCCESS;
}

static void delete_cb(struct sd_index *idx, void *arg, int ignore)
{
	struct sd_inode *inode = arg;
	uint64_t oid;
	int ret;

	if (idx->vdi_id) {
		oid = vid_to_data_oid(idx->vdi_id, idx->idx);
		if (idx->vdi_id != inode->vdi_id)
			sd_debug("object %" PRIx64 " is base's data, would"
				 " not be deleted.", oid);
		else {
			ret = sd_remove_object(oid);
			if (ret != SD_RES_SUCCESS)
				sd_err("remove object %" PRIx64 " fail, %d",
				       oid, ret);
		}
	}
}


struct delete_work {
	int ret;
	uint32_t vid;
	struct work work;
};

static int vdi_delete_horse(int32_t vid)
{
	struct sd_inode *inode = xvalloc(sizeof(*inode));
	int ret;

	ret = read_backend_object(vid_to_vdi_oid(vid),
				  (void *)inode, sizeof(*inode), 0);

	if (ret != SD_RES_SUCCESS) {
		sd_err("cannot read inode %"PRIx32, vid);
		goto out;
	}

	if (inode->vdi_size == 0 && vdi_is_deleted(inode))
		goto out;

	if (inode->store_policy == 0) {
		size_t nr_objs, i;

		nr_objs = count_data_objs(inode);
		for (i = 0; i < nr_objs; i++) {
			uint32_t vdi_id = sd_inode_get_vid(inode, i);
			uint64_t oid;

			if (!vdi_id)
				continue;

			oid = vid_to_data_oid(vdi_id, i);
			ret = sd_unref_object(oid, inode->gref[i].generation,
					      inode->gref[i].count);
			/*
			 * Return error if we fail to remove any object.
			 *
			 * We don't update inode to boost delete performance,
			 * users can continue deletion until success.
			 */
			if (ret != SD_RES_SUCCESS && ret != SD_RES_NO_OBJ) {
				sd_err("unref %" PRIx64 " fail, %d", oid, ret);
				goto out;
			}
		}
	} else {
		/*
		 * todo: generational reference counting is not supported by
		 * hypervolume yet
		 */
		sd_inode_index_walk(inode, delete_cb, inode);
	}

	/* We can't delete inode due to vdi allocation logic, just zero name. */
	inode->vdi_size = 0;
	memset(inode->name, 0, sizeof(inode->name));
	ret = sd_write_object(vid_to_vdi_oid(vid), (void *)inode,
			      SD_INODE_HEADER_SIZE, 0, false);
out:
	free(inode);
	return ret;
}

static void vdi_delete_work(struct work *work)
{
	struct delete_work *dw = container_of(work, struct delete_work, work);
	dw->ret = vdi_delete_horse(dw->vid);
}

static void vdi_delete_done(struct work *work)
{
	struct delete_work *dw = container_of(work, struct delete_work, work);

	if (dw->ret != SD_RES_SUCCESS)
		sd_err("failed to delete %"PRIx32 " %s",
		       dw->vid,  sd_strerror(dw->ret));
	free(dw);
}

/*
 * We need to put deletion into a worker thread otherwise we'll block
 * .process_work. So return of the deletion request only implies the success
 * of the acception of 'vdi delete' request. The real deletion is delayed and
 * could fail. In the failure case, users are expected to resend the deletion
 * request.
 *
 * In synchronous deletion, return of the deletion request means the completion
 * of the delete operation, both data and meta data are deleted. But this will
 * block the cluster operation.
 */
int vdi_delete(uint32_t vid, bool async_delete)
{
	struct delete_work *dw;

	if (!async_delete)
		return vdi_delete_horse(vid);

	dw = xzalloc(sizeof(*dw));
	dw->vid = vid;
	dw->work.fn = vdi_delete_work;
	dw->work.done = vdi_delete_done;
	queue_work(sys->deletion_wqueue, &dw->work);

	return SD_RES_SUCCESS;
}

/* Calculate a vdi attribute id from sheepdog_vdi_attr. */
static uint32_t hash_vdi_attr(const struct sheepdog_vdi_attr *attr)
{
	uint64_t hval;

	/* We cannot use sd_hash for backward compatibility. */
	hval = fnv_64a_buf(attr->name, sizeof(attr->name), FNV1A_64_INIT);
	hval = fnv_64a_buf(attr->tag, sizeof(attr->tag), hval);
	hval = fnv_64a_buf(&attr->snap_id, sizeof(attr->snap_id), hval);
	hval = fnv_64a_buf(attr->key, sizeof(attr->key), hval);

	return (uint32_t)(hval & ((UINT64_C(1) << VDI_SPACE_SHIFT) - 1));
}

int get_vdi_attr(struct sheepdog_vdi_attr *vattr, int data_len,
		 uint32_t vid, uint32_t *attrid, uint64_t create_time,
		 bool wr, bool excl, bool delete)
{
	struct sheepdog_vdi_attr tmp_attr;
	uint64_t oid;
	uint32_t end;
	int ret = SD_RES_NO_OBJ;

	vattr->ctime = create_time;

	*attrid = hash_vdi_attr(vattr);

	end = *attrid - 1;
	while (*attrid != end) {
		oid = vid_to_attr_oid(vid, *attrid);
		if (excl || !wr)
			ret = sd_read_object(oid, (char *)&tmp_attr,
					sizeof(tmp_attr), 0);

		if (ret == SD_RES_NO_OBJ && wr) {
			ret = sd_write_object(oid, (char *)vattr, data_len, 0,
					      true);
			if (ret)
				ret = SD_RES_EIO;
			else
				ret = SD_RES_SUCCESS;
			goto out;
		}

		if (ret != SD_RES_SUCCESS)
			goto out;

		/* compare attribute header */
		if (strcmp(tmp_attr.name, vattr->name) == 0 &&
		    strcmp(tmp_attr.tag, vattr->tag) == 0 &&
		    tmp_attr.snap_id == vattr->snap_id &&
		    strcmp(tmp_attr.key, vattr->key) == 0) {
			if (excl)
				ret = SD_RES_VDI_EXIST;
			else if (delete) {
				ret = sd_write_object(oid, (char *)"", 1,
				offsetof(struct sheepdog_vdi_attr, name),
						      false);
				if (ret)
					ret = SD_RES_EIO;
				else
					ret = SD_RES_SUCCESS;
			} else if (wr) {
				ret = sd_write_object(oid, (char *)vattr,
						      SD_ATTR_OBJ_SIZE, 0,
						      false);

				if (ret)
					ret = SD_RES_EIO;
				else
					ret = SD_RES_SUCCESS;
			} else
				ret = SD_RES_SUCCESS;
			goto out;
		}

		(*attrid)++;
	}

	sd_debug("there is no space for new VDIs");
	ret = SD_RES_FULL_VDI;
out:
	return ret;
}

void clean_vdi_state(void)
{
	sd_write_lock(&vdi_state_lock);
	rb_destroy(&vdi_state_root, struct vdi_state_entry, node);
	INIT_RB_ROOT(&vdi_state_root);
	sd_rw_unlock(&vdi_state_lock);
}

int sd_delete_vdi(const char *name)
{
	struct sd_req hdr;
	char data[SD_MAX_VDI_LEN] = {0};
	int ret;

	sd_init_req(&hdr, SD_OP_DEL_VDI);
	hdr.flags = SD_FLAG_CMD_WRITE;
	hdr.data_length = sizeof(data);
	pstrcpy(data, SD_MAX_VDI_LEN, name);

	ret = exec_local_req(&hdr, data);
	if (ret != SD_RES_SUCCESS)
		sd_err("Failed to delete vdi %s %s", name, sd_strerror(ret));

	return ret;
}

int sd_lookup_vdi(const char *name, uint32_t *vid)
{
	int ret;
	struct vdi_info info = {};
	struct vdi_iocb iocb = {
		.name = name,
		.data_len = strlen(name),
	};

	ret = vdi_lookup(&iocb, &info);
	switch (ret) {
	case SD_RES_SUCCESS:
		*vid = info.vid;
		break;
	case SD_RES_NO_VDI:
		break;
	default:
		sd_err("Failed to lookup name %s, %s", name, sd_strerror(ret));
	}

	return ret;
}

int sd_create_hyper_volume(const char *name, uint32_t *vdi_id)
{
	struct sd_req hdr;
	struct sd_rsp *rsp = (struct sd_rsp *)&hdr;
	char buf[SD_MAX_VDI_LEN] = {};
	int ret;

	pstrcpy(buf, SD_MAX_VDI_LEN, name);

	sd_init_req(&hdr, SD_OP_NEW_VDI);
	hdr.flags = SD_FLAG_CMD_WRITE;
	hdr.data_length = SD_MAX_VDI_LEN;

	hdr.vdi.vdi_size = SD_MAX_VDI_SIZE;
	hdr.vdi.copies = sys->cinfo.nr_copies;
	hdr.vdi.copy_policy = sys->cinfo.copy_policy;
	hdr.vdi.store_policy = 1;

	ret = exec_local_req(&hdr, buf);
	if (ret != SD_RES_SUCCESS) {
		sd_err("Failed to create VDI %s: %s", name, sd_strerror(ret));
		goto out;
	}

	if (vdi_id)
		*vdi_id = rsp->vdi.vdi_id;
out:
	return ret;
}
