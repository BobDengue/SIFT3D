/* -----------------------------------------------------------------------------
 * reg.c
 * -----------------------------------------------------------------------------
 * Copyright (c) 2015-2016 Blaine Rister et al., see LICENSE for details.
 * -----------------------------------------------------------------------------
 * This file contains routines for image registration.
 * -----------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "reg.h"
#include "types.h"
#include "macros.h"
#include "sift.h"
#include "imutil.h"

/* Default parameters */
const double SIFT3D_nn_thresh_default = 0.8; // Default matching threshold

/* Internal helper routines */
static void scale_SIFT3D(const double *const factors, 
	Keypoint_store *const kp, SIFT3D_Descriptor_store *const d);
static int im2mm(const Mat_rm *const im, const double *const units, 
        Mat_rm *const mm);
static int mm2im(const double *const src_units, const double *const ref_units,
        void *const tform);

/* Convert an [mxIM_NDIMS] coordinate matrix from image space to mm. 
 *
 * Parameters:
 *   im: The input coordinate matrix, in image space. 
 *   units: An array of length IM_NDIMS giving the units of image space.
 *   mm: The output coordinate matrix, in mm.
 *
 * Returns SIFT3D_SUCCESS on success, SIFT3D_FAILURE otherwise.
 */
static int im2mm(const Mat_rm *const im, const double *const units, 
        Mat_rm *const mm) {

        int i, j;

        // Verify inputs
        if (im->num_cols != IM_NDIMS) {
                fputs("im2mm: input must have IM_NDIMS columns. \n", stderr);
                return SIFT3D_FAILURE;
        }
        if (im->type != DOUBLE) {
                fputs("im2mm: input must have type double. \n", stderr);
                return SIFT3D_FAILURE;
        }

        // Copy the input 
        if (copy_Mat_rm(im, mm))
                return SIFT3D_FAILURE;

        // Convert the units
        SIFT3D_MAT_RM_LOOP_START(mm, i, j)
                SIFT3D_MAT_RM_GET(mm, i, j, double) *= units[j];
        SIFT3D_MAT_RM_LOOP_END 

        return SIFT3D_SUCCESS;
}

/* Convert a transformation from mm to image space.
 *
 * Parameters:
 *   tform: The transformation, which shall be modified.
 *   src_units: The units of the source image, array of length IM_NDIMS.
 *   ref_units: As src_units, but for the reference image.
 *
 * Returns SIFT3D_SUCCESS on success, SIFT3D_FAILURE otherwise.
 */
static int mm2im(const double *const src_units, const double *const ref_units,
        void *const tform) {

        const tform_type type = tform_get_type(tform);

        switch (type) {
        case AFFINE:
                { 
                        int i, j;

                        Affine *const aff = (Affine *const) tform;
                        Mat_rm *const A = &aff->A;

                        // Verify the dimensions
                        if (A->num_rows != IM_NDIMS) {
                                fprintf(stderr, "mm2im: Invalid transform "
                                        "dimensionality: %d \n", A->num_rows);
                                return SIFT3D_FAILURE;
                        }

                        // Convert the Affine transformation matrix in-place
                        SIFT3D_MAT_RM_LOOP_START(A, i, j)
                                // Invert the input transformation ref->mm
                                SIFT3D_MAT_RM_GET(A, i, j, double) *= 
                                        j < IM_NDIMS ? ref_units[j] : 1.0;

                                // Invert the output transformation src->mm
                                SIFT3D_MAT_RM_GET(A, i, j, double) /= 
                                        src_units[i];
                        SIFT3D_MAT_RM_LOOP_END
                }
                break; 
        default:
                fputs("mm2im: unsupported transform type. \n", stderr);
                return SIFT3D_FAILURE;
        }

        return SIFT3D_SUCCESS; 
}

/* Initialize a Reg_SIFT3D struct with the default parameters. This must be
 * called before the struct can be used. */
int init_Reg_SIFT3D(Reg_SIFT3D *const reg) {

	reg->matches = NULL;
        reg->nn_thresh = SIFT3D_nn_thresh_default;
	init_Keypoint_store(&reg->kp_src);
	init_Keypoint_store(&reg->kp_ref);
	init_SIFT3D_Descriptor_store(&reg->desc_src);
	init_SIFT3D_Descriptor_store(&reg->desc_ref);
	init_Ransac(&reg->ran);
	if (init_SIFT3D(&reg->sift3d) ||
                init_Mat_rm(&reg->match_src, 0, 0, DOUBLE, SIFT3D_FALSE) ||
		init_Mat_rm(&reg->match_ref, 0, 0, DOUBLE, SIFT3D_FALSE)) {
                fprintf(stderr, "register_SIFT3D: unexpected error \n");
                return SIFT3D_FAILURE;
        }

        return SIFT3D_SUCCESS;
}

/* Free all memory associated with a Reg_SIFT3D struct. reg cannot be reused
 * unless it is reinitialized. */
void cleanup_Reg_SIFT3D(Reg_SIFT3D *const reg) {

        if (reg->matches != NULL)
                free(reg->matches);

        cleanup_Keypoint_store(&reg->kp_src);
        cleanup_Keypoint_store(&reg->kp_ref);
        cleanup_SIFT3D_Descriptor_store(&reg->desc_src);
        cleanup_SIFT3D_Descriptor_store(&reg->desc_ref);
        cleanup_SIFT3D(&reg->sift3d); 
        cleanup_Mat_rm(&reg->match_src);
        cleanup_Mat_rm(&reg->match_ref);
}

/* Set the matching theshold of a Reg_SIFT3D struct. */
int set_nn_thresh_Reg_SIFT3D(Reg_SIFT3D *const reg, const double nn_thresh) {

        if (nn_thresh <= 0 || nn_thresh > 1) {
                fprintf(stderr, "set_nn_thresh_Reg_SIFT3D: invalid threshold: "
                        "%f \n", nn_thresh);
                return SIFT3D_FAILURE;
        }

        reg->nn_thresh = nn_thresh;
        return SIFT3D_SUCCESS;
}

/* Set the Ransac parameters of the Reg_SIFT3D struct. */
int set_Ransac_Reg_SIFT3D(Reg_SIFT3D *const reg, const Ransac *const ran) {
        return copy_Ransac(ran, &reg->ran);
}

/* Set the SIFT3D parameters of the Reg_SIFT3D struct. Makes a deep copy of
 * sift3d, so you are free to modify it after calling this function. */
int set_SIFT3D_Reg_SIFT3D(Reg_SIFT3D *const reg, const SIFT3D *const sift3d) {
        return copy_SIFT3D(sift3d, &reg->sift3d);
}

/* Set the source image. This makes a deep copy of the data, so you are free
 * to modify src after calling this function. */
int set_src_Reg_SIFT3D(Reg_SIFT3D *const reg, const Image *const src) {

        SIFT3D *const sift3d = &reg->sift3d;
        Keypoint_store *const kp_src = &reg->kp_src;
        SIFT3D_Descriptor_store *const desc_src = &reg->desc_src;

        // Save the units
        memcpy(reg->src_units, SIFT3D_IM_GET_UNITS(src), 
                IM_NDIMS * sizeof(double));

        // Detect keypoints
	if (SIFT3D_detect_keypoints(sift3d, src, kp_src)) {
		fprintf(stderr, "set_src_Reg_SIFT3D: failed to detect source "
                        "keypoints\n");
                return SIFT3D_FAILURE;
        }

        // Extract descriptors
	if (SIFT3D_extract_descriptors(sift3d, kp_src, desc_src)) {
                fprintf(stderr, "set_ref_Reg_SIFT3D: failed to extract source "
                                "descriptors \n");
                return SIFT3D_FAILURE;
        }

        return SIFT3D_SUCCESS;
}

/* The same as set_source_Reg_SIFT3D, but sets the reference image. */
int set_ref_Reg_SIFT3D(Reg_SIFT3D *const reg, const Image *const ref) {

        SIFT3D *const sift3d = &reg->sift3d;
        Keypoint_store *const kp_ref = &reg->kp_ref;
        SIFT3D_Descriptor_store *const desc_ref = &reg->desc_ref;

        // Save the units
        memcpy(reg->ref_units, SIFT3D_IM_GET_UNITS(ref), 
                IM_NDIMS * sizeof(double));

        // Detect keypoints
        if (SIFT3D_detect_keypoints(sift3d, ref, kp_ref)) {
		fprintf(stderr, "set_ref_Reg_SIFT3D: failed to detect "
			"reference keypoints\n");
                return SIFT3D_FAILURE;
        }

        // Extract descriptors
	if (SIFT3D_extract_descriptors(sift3d, kp_ref, desc_ref)) {
		fprintf(stderr, "set_ref_Reg_SIFT3D: failed to extract "
			"reference descriptors\n");
                return SIFT3D_FAILURE;
        }

        return SIFT3D_SUCCESS;
}

/* Run the registration procedure. 
 *
 * Parameters: 
 *   reg: The struct holding registration state.
 *   tform: The output transformation. If NULL, this function only performs
 *     feature matching.
 *
 * Returns SIFT3D_SUCCESS on success, SIFT3D_FAILURE otherwise. */
int register_SIFT3D(Reg_SIFT3D *const reg, void *const tform) {

        Mat_rm match_src_mm, match_ref_mm;
        int i, j;

        Ransac *const ran = &reg->ran;
        Mat_rm *const match_src = &reg->match_src;
        Mat_rm *const match_ref = &reg->match_ref;
        int **const matches = &reg->matches;
        const double nn_thresh = reg->nn_thresh;
        SIFT3D_Descriptor_store *const desc_src = &reg->desc_src;
        SIFT3D_Descriptor_store *const desc_ref = &reg->desc_ref;

	// Verify inputs
	if (desc_src->num <= 0) {
		fprintf(stderr, "register_SIFT3D: no source image descriptors "
			"are available \n");
		return SIFT3D_FAILURE;
	}
	if (desc_ref->num <= 0) {
		fprintf(stderr, "register_SIFT3D: no reference image "
			"descriptors are available \n");
		return SIFT3D_FAILURE;
	}

	// Match features
	if (SIFT3D_nn_match_fb(desc_src, desc_ref, nn_thresh, matches)) {
		fprintf(stderr, "register_SIFT3D: failed to match "
                                "descriptors \n");
                return SIFT3D_FAILURE;
        }

        // Convert matches to coordinate matrices
	if (SIFT3D_matches_to_Mat_rm(desc_src, desc_ref, *matches,
				     match_src, match_ref)) {
		fprintf(stderr, "register_SIFT3D: failed to extract "
                                "coordinate matrices \n");
                return SIFT3D_FAILURE;
        }

        // Quit if no tform was provided
        if (tform == NULL)
                return SIFT3D_SUCCESS;

        // Initialize intermediates
        if (init_Mat_rm(&match_src_mm, 0, 0, DOUBLE, SIFT3D_FALSE) ||
	        init_Mat_rm(&match_ref_mm, 0, 0, DOUBLE, SIFT3D_FALSE)) {
                fprintf(stderr, "register_SIFT3D: failed initialization \n");
                return SIFT3D_FAILURE;
        }

        // Convert the coordinate matrices to real-world units
        if (im2mm(match_src, reg->src_units, &match_src_mm) ||
            im2mm(match_ref, reg->ref_units, &match_ref_mm))
                goto register_SIFT3D_quit;

	// Find the transformation in real-world units
	if (find_tform_ransac(ran, &match_src_mm, &match_ref_mm, tform))
                goto register_SIFT3D_quit;

        // Convert the transformation back to image space
        if (mm2im(reg->src_units, reg->ref_units, tform))
                goto register_SIFT3D_quit;

        // Clean up
        cleanup_Mat_rm(&match_src_mm);
        cleanup_Mat_rm(&match_ref_mm);

	return SIFT3D_SUCCESS;

register_SIFT3D_quit:
        cleanup_Mat_rm(&match_src_mm); 
        cleanup_Mat_rm(&match_ref_mm); 
        return SIFT3D_FAILURE;
}

/* Helper function to scale the keypoints and descriptors by the given 
 * factors */
static void scale_SIFT3D(const double *const factors, 
	Keypoint_store *const kp, SIFT3D_Descriptor_store *const d) {

	double det, scale_factor;
	int i, j, k;

	// Compute the determinant of the scaling transformation
	det = 1.0;
	for (i = 0; i < IM_NDIMS; i++) {
		det *= factors[i];	
	}

	// Compute the scale parameter factor from the determinant
	scale_factor = pow(det, -1.0 / (double) IM_NDIMS);
	
	// Scale the keypoints
	for (k = 0; k < kp->slab.num; k++) {

		Keypoint *const key = kp->buf + k;
		Mat_rm *const R = &key->R;

		// Scale the coordinates
		key->xd *= factors[0];
		key->yd *= factors[1];
		key->zd *= factors[2];

		// Adjust the scale parameter
		key->sd *= scale_factor;

		// Adjust the orientation matrix
		SIFT3D_MAT_RM_LOOP_START(R, i, j)
			SIFT3D_MAT_RM_GET(R, i, j, float) *= 
				(float) (factors[j] / det);
		SIFT3D_MAT_RM_LOOP_END
	}

	// Scale the descriptors
	for (i = 0; i < d->num; i++) {

		SIFT3D_Descriptor *const desc = d->buf + i;

		// Scale the coordinates
		desc->xd *= factors[0];
		desc->yd *= factors[1];
		desc->zd *= factors[2];

		// Adjust the scale parameter
		desc->sd *= scale_factor;
	}
}

/* Like register_SIFT3D, but resamples the input images to have the same
 * physical resolution before extracting features. Use this when registering 
 * images with very different resolutions. The results are converted to the
 * original resolution.
 *
 * Parameters:
 *   reg: See register_SIFT3D.
 *   src: The source, or moving image.
 *   ref: The reference, or fixed image.
 *   interp: The type of interpolation to use.
 *   tform: See register_SIFT3D. 
 *
 * Note that some fields of the returned keypoints, such as scale and
 * orientation, will not make sense in the new coordinate system.
 *
 * Returns SIFT3D_SUCCESS on success, SIFT3D_FAILURE otherwise. */
int register_SIFT3D_resample(Reg_SIFT3D *const reg, const Image *const src,
	const Image *const ref, const interp_type interp, void *const tform) {

	double units_min[IM_NDIMS], factors_src[IM_NDIMS], 
		factors_ref[IM_NDIMS];
	Image src_interp, ref_interp;
	int i;

	// Check for the trivial case, when src and dst have the same units
	if (!memcmp(SIFT3D_IM_GET_UNITS(src), SIFT3D_IM_GET_UNITS(ref), 
		IM_NDIMS * sizeof(double))) {
		return set_src_Reg_SIFT3D(reg, src) ||
			set_ref_Reg_SIFT3D(reg, ref) ||
			register_SIFT3D(reg, tform) ? 
			SIFT3D_FAILURE : SIFT3D_SUCCESS;
	}

	// Initalize intermediates
	init_im(&src_interp);
	init_im(&ref_interp);

	// Compute the new units and scaling factors
	for (i = 0; i < IM_NDIMS; i++) {
		const double unit_src = SIFT3D_IM_GET_UNITS(src)[i];
		const double unit_ref = SIFT3D_IM_GET_UNITS(ref)[i];

		// Compute the minimum units between the two images
		units_min[i] = SIFT3D_MIN(unit_src, unit_ref);

		// Compute the scaling factors between the interpolated
		// images and the originals
		factors_src[i] = units_min[i] / unit_src;
		factors_ref[i] = units_min[i] / unit_ref;
	}

	// Resample the images
	if (im_resample(src, units_min, interp, &src_interp) ||
		im_resample(ref, units_min, interp, &ref_interp))
		goto register_interp_quit;

	// Extract features from the interpolated images
	if (set_src_Reg_SIFT3D(reg, &src_interp) ||
		set_ref_Reg_SIFT3D(reg, &ref_interp))
		goto register_interp_quit;

	// Convert the keypoints and descriptors to the original units
	scale_SIFT3D(factors_src, &reg->kp_src, &reg->desc_src);
	scale_SIFT3D(factors_ref, &reg->kp_ref, &reg->desc_ref);

	// Register the images
	if (register_SIFT3D(reg, tform))
		goto register_interp_quit;

	// Clean up
	im_free(&src_interp);	
	im_free(&ref_interp);	

	return SIFT3D_SUCCESS;

register_interp_quit:
	im_free(&src_interp);	
	im_free(&ref_interp);	
	return SIFT3D_FAILURE;
}

/* Write the coordinates of matching keypoints to the matrices match_src
 * and match_ref. This function uses the keypoints and the matches from
 * the last call to register_SIFT3D() on this Reg_SIFT3D struct. */
int get_matches_Reg_SIFT3D(const Reg_SIFT3D *const reg, Mat_rm *const match_src,
        Mat_rm *const match_ref) {

        // Check if we have any matches
        if (reg->matches == NULL)
                return SIFT3D_FAILURE;

        // Copy the matches
        return copy_Mat_rm(&reg->match_src, match_src) ||
                copy_Mat_rm(&reg->match_ref, match_ref);
}

