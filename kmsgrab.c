// SPDX-License-Identifier: GPL-2.0-only
/*
 * KMS/DRM screenshot tool
 *
 * Copyright (c) 2021 Paul Cercueil <paul@crapouillou.net>
 */

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <png.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

typedef struct {
	uint8_t r, g, b;
} uint24_t;

static inline uint24_t rgb16_to_24(uint16_t px)
{
	uint24_t pixel;

	pixel.b = (px & 0x1f)   << 3;
	pixel.g = (px & 0x7e0)  >> 3;
	pixel.r = (px & 0xf800) >> 8;

	return pixel;
}


static inline uint24_t rgb32_to_24(uint32_t px)
{
	uint24_t pixel;

	pixel.b = px & 0xff;
	pixel.g = (px >> 8) & 0xff;
	pixel.r = (px >> 16) & 0xff;

	return pixel;
}


static inline void convert_to_24(drmModeFB *fb, uint24_t *to, void *from)
{
	unsigned int len = fb->width * fb->height;

	if (fb->bpp == 16) {
		uint16_t *ptr = from;
		while (len--)
			*to++ = rgb16_to_24(*ptr++);
	} else {
		uint32_t *ptr = from;
		while (len--)
			*to++ = rgb32_to_24(*ptr++);
	}
}

static int save_png(drmModeFB2 *fb2, int drm_fd, const char *png_fn, int plane)
{
	png_bytep *row_pointers;
	png_structp png;
	png_infop info;
	FILE *pngfile;
	void *buffer, *picture;
	unsigned int size, p;
	int ret, err, prime_fd;
	char filename[64];

	sprintf(filename, "%s-%u.raw", png_fn, plane);
	pngfile = fopen(filename, "w+");
	if (!pngfile) {
		fprintf(stderr, "Failed to open output file: %s\n",
			strerror(-err));
		ret = -errno;
		return ret;
	}

	for (p = 0; p < 4; p++) {
		if (!fb2->handles[p])
			continue;
		err = drmPrimeHandleToFD(drm_fd, fb2->handles[p], O_RDONLY, &prime_fd);
		if (err < 0) {
			continue;
		}

		// Assume planes are subsampled as I can't think of the function calls
		// to get this information in userspace
		size = fb2->pitches[p] * fb2->height / (!p ? 1 : 2);
		buffer = mmap(NULL, size,
			      PROT_READ, MAP_PRIVATE, prime_fd, 0);
		if (buffer == MAP_FAILED) {
			ret = -errno;
			fprintf(stderr, "Unable to mmap prime buffer plane %u\n", p);
		} else {
			fwrite(buffer, size, 1, pngfile);

			munmap(buffer, size);
		}
		close(prime_fd);
	}
	fclose(pngfile);
	return ret;
}

int main(int argc, char **argv)
{
	int err, drm_fd, prime_fd, retval = EXIT_FAILURE;
	unsigned int i, card;
	uint32_t fb_id, crtc_id;
	drmModePlaneRes *plane_res;
	drmModePlane *plane;
	drmModeFB *fb;
	drmModeFB2Ptr fb2;
	char buf[256];
	uint64_t has_dumb;

	if (argc < 2) {
		printf("Usage: kmsgrab <output.png>\n");
		goto out_return;
	}

	for (card = 0; ; card++) {
		snprintf(buf, sizeof(buf), "/dev/dri/card%u", card);

		drm_fd = open(buf, O_RDWR | O_CLOEXEC);
		if (drm_fd < 0) {
			fprintf(stderr, "Could not open KMS/DRM device.\n");
			goto out_return;
		}

		if (drmGetCap(drm_fd, DRM_CAP_DUMB_BUFFER, &has_dumb) >= 0 &&
		    has_dumb)
			break;

		close(drm_fd);
	}

	drm_fd = open(buf, O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		fprintf(stderr, "Could not open KMS/DRM device.\n");
		goto out_return;
	}

	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		fprintf(stderr, "Unable to set atomic cap.\n");
		goto out_close_fd;
	}

	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
		fprintf(stderr, "Unable to set universal planes cap.\n");
		goto out_close_fd;
	}

	plane_res = drmModeGetPlaneResources(drm_fd);
	if (!plane_res) {
		fprintf(stderr, "Unable to get plane resources.\n");
		goto out_close_fd;
	}

	for (i = 0; i < plane_res->count_planes; i++) {
		plane = drmModeGetPlane(drm_fd, plane_res->planes[i]);
		fb_id = plane->fb_id;
		crtc_id = plane->crtc_id;
		drmModeFreePlane(plane);

		if (!fb_id || !crtc_id)
			continue;


		fb2 = drmModeGetFB2(drm_fd, fb_id);
		if (!fb2) {
			fprintf(stderr, "Failed to get framebuffer %"PRIu32": %s\n",
				fb_id, strerror(errno));
			goto out_free_resources;
		}

		err = save_png(fb2, drm_fd, argv[1], i);
		if (err < 0) {
			fprintf(stderr, "Failed to take screenshot: %s\n",
				strerror(-err));
			goto out_close_prime_fd;
		}
	}

	retval = EXIT_SUCCESS;

out_close_prime_fd:
	close(prime_fd);
out_free_fb:
	drmModeFreeFB(fb);
out_free_resources:
	drmModeFreePlaneResources(plane_res);
out_close_fd:
	close(drm_fd);
out_return:
	return retval;
}
