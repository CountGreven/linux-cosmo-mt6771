// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek CAMSV capture video node
 *
 * The format follows the pad format of the sub-device: the node is configured
 * through the media graph. The hardware writes the 10 bit value in bits 13:4
 * of a 16 bit word, which is a 14 bit sample.
 */

#include <linux/dma-mapping.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mc.h>
#include <media/videobuf2-dma-contig.h>

#include "mtk-camsv.h"

#define CAMSV_DMA_LIMIT		SZ_4G

/* Layout */

static void mtk_camsv_get_mbus_fmt(struct mtk_camsv *priv, struct v4l2_mbus_framefmt *fmt)
{
	struct v4l2_subdev_state *state = v4l2_subdev_lock_and_get_active_state(&priv->sd);

	*fmt = *v4l2_subdev_state_get_format(state, CAMSV_PAD_SOURCE);
	v4l2_subdev_unlock_state(state);
}

static void mtk_camsv_fill_pix(struct v4l2_pix_format *pix, const struct v4l2_mbus_framefmt *fmt)
{
	const struct mtk_camsv_format *f = mtk_camsv_format_by_code(fmt->code);

	if (!f)
		f = &mtk_camsv_formats[ARRAY_SIZE(mtk_camsv_formats) - 1];

	memset(pix, 0, sizeof(*pix));
	pix->width = fmt->width;
	pix->height = fmt->height;
	pix->pixelformat = f->fourcc;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_RAW;
	pix->bytesperline = pix->width * 2;
	pix->sizeimage = pix->bytesperline * pix->height;
}

/* vb2 */

static int mtk_camsv_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
				 unsigned int *nplanes, unsigned int sizes[],
				 struct device *alloc_devs[])
{
	struct mtk_camsv *priv = vb2_get_drv_priv(vq);
	struct v4l2_mbus_framefmt fmt;
	struct v4l2_pix_format pix;

	mtk_camsv_get_mbus_fmt(priv, &fmt);
	mtk_camsv_fill_pix(&pix, &fmt);

	if (*nplanes)
		return sizes[0] < pix.sizeimage ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = pix.sizeimage;

	return 0;
}

static int mtk_camsv_buf_prepare(struct vb2_buffer *vb)
{
	struct mtk_camsv *priv = vb2_get_drv_priv(vb->vb2_queue);
	struct mtk_camsv_buffer *buf = container_of(to_vb2_v4l2_buffer(vb),
						    struct mtk_camsv_buffer, vb);
	struct v4l2_mbus_framefmt fmt;
	struct v4l2_pix_format pix;

	mtk_camsv_get_mbus_fmt(priv, &fmt);
	mtk_camsv_fill_pix(&pix, &fmt);

	if (vb2_plane_size(vb, 0) < pix.sizeimage)
		return -EINVAL;

	buf->addr = vb2_dma_contig_plane_dma_addr(vb, 0);
	/* The base address register is 32 bit and the block has no IOMMU yet */
	if (buf->addr + pix.sizeimage > CAMSV_DMA_LIMIT)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, pix.sizeimage);

	if (priv->dbg_poison)
		memset(vb2_plane_vaddr(vb, 0), priv->dbg_poison, pix.sizeimage);

	return 0;
}

static void mtk_camsv_buf_queue(struct vb2_buffer *vb)
{
	struct mtk_camsv *priv = vb2_get_drv_priv(vb->vb2_queue);
	struct mtk_camsv_buffer *buf = container_of(to_vb2_v4l2_buffer(vb),
						    struct mtk_camsv_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&priv->qlock, flags);
	list_add_tail(&buf->list, &priv->buffers);
	spin_unlock_irqrestore(&priv->qlock, flags);
}

static void mtk_camsv_return_buffers(struct mtk_camsv *priv, enum vb2_buffer_state state)
{
	struct mtk_camsv_buffer *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&priv->qlock, flags);
	if (priv->active) {
		vb2_buffer_done(&priv->active->vb.vb2_buf, state);
		priv->active = NULL;
	}
	if (priv->next) {
		vb2_buffer_done(&priv->next->vb.vb2_buf, state);
		priv->next = NULL;
	}
	list_for_each_entry_safe(buf, tmp, &priv->buffers, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&priv->qlock, flags);
}

static int mtk_camsv_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct mtk_camsv *priv = vb2_get_drv_priv(q);
	struct v4l2_mbus_framefmt fmt;
	struct v4l2_pix_format pix;
	struct media_pad *remote;
	unsigned long flags;
	int ret;

	mtk_camsv_get_mbus_fmt(priv, &fmt);
	mtk_camsv_fill_pix(&pix, &fmt);

	ret = video_device_pipeline_alloc_start(&priv->vdev);
	if (ret)
		goto err_buffers;

	remote = media_pad_remote_pad_first(&priv->pads[CAMSV_PAD_SINK]);
	if (!remote || !is_media_entity_v4l2_subdev(remote->entity)) {
		ret = -ENOLINK;
		goto err_pipeline;
	}

	/* Frames that find no buffer go here */
	priv->dummy_size = pix.sizeimage;
	priv->dummy_cpu = dma_alloc_coherent(priv->dev, priv->dummy_size, &priv->dummy_dma,
					     GFP_KERNEL);
	if (!priv->dummy_cpu) {
		ret = -ENOMEM;
		goto err_pipeline;
	}

	/* Preconditions in order: domain, larbs and clocks; only then registers */
	ret = pm_runtime_resume_and_get(priv->dev);
	if (ret < 0)
		goto err_dummy;

	spin_lock_irqsave(&priv->qlock, flags);
	priv->active = list_first_entry_or_null(&priv->buffers, struct mtk_camsv_buffer, list);
	if (priv->active)
		list_del(&priv->active->list);
	spin_unlock_irqrestore(&priv->qlock, flags);

	ret = mtk_camsv_hw_start(priv, &fmt, pix.bytesperline);
	if (ret)
		goto err_put;

	ret = v4l2_subdev_enable_streams(media_entity_to_v4l2_subdev(remote->entity),
					 remote->index, BIT_ULL(0));
	if (ret)
		goto err_hw;

	return 0;

err_hw:
	mtk_camsv_hw_stop(priv);
err_put:
	pm_runtime_put(priv->dev);
err_dummy:
	dma_free_coherent(priv->dev, priv->dummy_size, priv->dummy_cpu, priv->dummy_dma);
	priv->dummy_cpu = NULL;
err_pipeline:
	video_device_pipeline_stop(&priv->vdev);
err_buffers:
	mtk_camsv_return_buffers(priv, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void mtk_camsv_stop_streaming(struct vb2_queue *q)
{
	struct mtk_camsv *priv = vb2_get_drv_priv(q);
	struct media_pad *remote = media_pad_remote_pad_first(&priv->pads[CAMSV_PAD_SINK]);

	/* Frame flow first, at a frame boundary, then the sensor, then the rest */
	mtk_camsv_hw_vf_off(priv);
	if (remote)
		v4l2_subdev_disable_streams(media_entity_to_v4l2_subdev(remote->entity),
					    remote->index, BIT_ULL(0));
	mtk_camsv_hw_stop(priv);
	pm_runtime_put(priv->dev);

	dma_free_coherent(priv->dev, priv->dummy_size, priv->dummy_cpu, priv->dummy_dma);
	priv->dummy_cpu = NULL;
	video_device_pipeline_stop(&priv->vdev);
	mtk_camsv_return_buffers(priv, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops mtk_camsv_vb2_ops = {
	.queue_setup = mtk_camsv_queue_setup,
	.buf_prepare = mtk_camsv_buf_prepare,
	.buf_queue = mtk_camsv_buf_queue,
	.start_streaming = mtk_camsv_start_streaming,
	.stop_streaming = mtk_camsv_stop_streaming,
};

/* ioctls */

static int mtk_camsv_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	struct mtk_camsv *priv = video_drvdata(file);

	strscpy(cap->driver, "mtk-camsv", sizeof(cap->driver));
	strscpy(cap->card, "MediaTek CAMSV0", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s", dev_name(priv->dev));

	return 0;
}

static int mtk_camsv_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	const struct mtk_camsv_format *fmt;

	if (f->mbus_code) {
		fmt = mtk_camsv_format_by_code(f->mbus_code);
		if (!fmt || f->index)
			return -EINVAL;
	} else {
		if (f->index >= ARRAY_SIZE(mtk_camsv_formats))
			return -EINVAL;
		fmt = &mtk_camsv_formats[f->index];
	}

	f->pixelformat = fmt->fourcc;
	f->flags = 0;

	return 0;
}

static int mtk_camsv_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct mtk_camsv *priv = video_drvdata(file);
	struct v4l2_mbus_framefmt fmt;

	mtk_camsv_get_mbus_fmt(priv, &fmt);
	mtk_camsv_fill_pix(&f->fmt.pix, &fmt);

	return 0;
}

static const struct v4l2_ioctl_ops mtk_camsv_ioctl_ops = {
	.vidioc_querycap = mtk_camsv_querycap,
	.vidioc_enum_fmt_vid_cap = mtk_camsv_enum_fmt,
	.vidioc_g_fmt_vid_cap = mtk_camsv_g_fmt,
	.vidioc_s_fmt_vid_cap = mtk_camsv_g_fmt,
	.vidioc_try_fmt_vid_cap = mtk_camsv_g_fmt,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations mtk_camsv_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

int mtk_camsv_video_register(struct mtk_camsv *priv)
{
	struct video_device *vdev = &priv->vdev;
	struct vb2_queue *q = &priv->queue;
	int ret;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = priv;
	q->buf_struct_size = sizeof(struct mtk_camsv_buffer);
	q->ops = &mtk_camsv_vb2_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 2;
	q->lock = &priv->lock;
	q->dev = priv->dev;
	ret = vb2_queue_init(q);
	if (ret)
		return ret;

	vdev->v4l2_dev = &priv->v4l2_dev;
	vdev->fops = &mtk_camsv_fops;
	vdev->ioctl_ops = &mtk_camsv_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &priv->lock;
	vdev->queue = q;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
	strscpy(vdev->name, "mtk-camsv0 capture", sizeof(vdev->name));
	video_set_drvdata(vdev, priv);

	priv->vdev_pad.flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
	vdev->entity.function = MEDIA_ENT_F_IO_V4L;
	ret = media_entity_pads_init(&vdev->entity, 1, &priv->vdev_pad);
	if (ret)
		return ret;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_entity;

	ret = media_create_pad_link(&priv->sd.entity, CAMSV_PAD_SOURCE, &vdev->entity, 0,
				    MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (ret)
		goto err_video;

	return 0;

err_video:
	video_unregister_device(vdev);
err_entity:
	media_entity_cleanup(&vdev->entity);
	return ret;
}

void mtk_camsv_video_unregister(struct mtk_camsv *priv)
{
	video_unregister_device(&priv->vdev);
	media_entity_cleanup(&priv->vdev.entity);
}
