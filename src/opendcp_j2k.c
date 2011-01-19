/* 
     OpenDCP: Builds Digital Cinema Packages
     Copyright (c) 2010 Terrence Meiczinger, All Rights Reserved
 
     This program is free software: you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published by
     the Free Software Foundation, either version 3 of the License, or
     (at your option) any later version.

     This program is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
     GNU General Public License for more details.

     You should have received a copy of the GNU General Public License
     along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <openjpeg.h>
#include <omp.h>
#include "opendcp.h"
#include "image/opendcp_image.h"

static int initialize_4K_poc(opj_poc_t *POC, int numres){
    POC[0].tile  = 1; 
    POC[0].resno0  = 0; 
    POC[0].compno0 = 0;
    POC[0].layno1  = 1;
    POC[0].resno1  = numres-1;
    POC[0].compno1 = 3;
    POC[0].prg1 = CPRL;
    POC[1].tile  = 1;
    POC[1].resno0  = numres-1; 
    POC[1].compno0 = 0;
    POC[1].layno1  = 1;
    POC[1].resno1  = numres;
    POC[1].compno1 = 3;
    POC[1].prg1 = CPRL;
    return 2;
}

void set_cinema_encoder_parameters(context_t *context, opj_cparameters_t *parameters){
    parameters->tile_size_on = false;
    parameters->cp_tdx=1;
    parameters->cp_tdy=1;

    /* Tile part */
    parameters->tp_flag = 'C';
    parameters->tp_on = 1;

    /* Tile and Image shall be at (0,0) */
    parameters->cp_tx0 = 0;
    parameters->cp_ty0 = 0;
    parameters->image_offset_x0 = 0;
    parameters->image_offset_y0 = 0;

    /*Codeblock size= 32*32*/
    parameters->cblockw_init = 32;	
    parameters->cblockh_init = 32;
    parameters->csty |= 0x01;

    /* The progression order shall be CPRL */
    parameters->prog_order = CPRL;

    /* No ROI */
    parameters->roi_compno = -1;

    parameters->subsampling_dx = 1;
    parameters->subsampling_dy = 1;

    /* 9-7 transform */
    parameters->irreversible = 1;

    parameters->tcp_rates[0] = 0;
    parameters->tcp_numlayers++;
    parameters->cp_disto_alloc = 1;

    parameters->cp_rsiz = context->cinema_profile;
    if ( context->cinema_profile == DCP_CINEMA4K ) {
            parameters->numpocs = initialize_4K_poc(parameters->POC,parameters->numresolution);
    }
}

int check_image_compliance(context_t *context, odcp_image_t *image) {
    switch (context->cinema_profile) {
        case DCP_CINEMA2K:
            if (!((image->w == 2048) | (image->h == 1080))) {
                return DCP_FATAL;
            }
	    break;
        case DCP_CINEMA4K:
            if (!((image->w == 4096) | (image->h == 2160))) {
                return DCP_FATAL;
            }
            break;
        default:
            break;
    }
 
    return DCP_SUCCESS;
}

int convert_to_j2k(context_t *context, char *in_file, char *out_file, char *tmp_path) {
    odcp_image_t *odcp_image;
    int result;
	
	if (tmp_path == NULL) {
		tmp_path = "./";
	}
    dcp_log(LOG_DEBUG,"Reading input file %s",in_file);
    #pragma omp critical
    {
        read_tif(&odcp_image, in_file,0);
    }
	
     if (!odcp_image) {
        dcp_log(LOG_ERROR,"Unable to load tiff file %s",in_file);
        return DCP_FATAL;
     }

    // verify image is dci compliant 
     if (check_image_compliance(context, odcp_image) != DCP_SUCCESS) {
         dcp_log(LOG_ERROR,"Image %s is not DCI Compliant",in_file);
         return DCP_FATAL;
     }
    
    if (context->xyz) {
        dcp_log(LOG_INFO,"RGB->XYZ color conversion %s",in_file);
        if (rgb_to_xyz(odcp_image)) {
            dcp_log(LOG_ERROR,"Color conversion failed %s",in_file);
            return DCP_FATAL;
        }
    }

    if ( context->encoder == J2K_KAKADU ) {
        char tempfile[255];
        int n = omp_get_thread_num(); 
        sprintf(tempfile,"%stmp%d%s",tmp_path,n,"file.tif");
		result = write_tif(odcp_image,tempfile,0);
        
	if (result != DCP_SUCCESS) {
            dcp_log(LOG_ERROR,"Writing temporary tif failed");
            return DCP_FATAL;
        }
	result = encode_kakadu(context, tempfile, out_file, tmp_path);
	if ( result != DCP_SUCCESS) {
            dcp_log(LOG_ERROR,"Kakadu JPEG2000 conversion failed %s",in_file);
            remove(tempfile);
            return DCP_FATAL;
        }
        remove(tempfile);
    } else {
        if (encode_openjpeg(context,odcp_image,out_file) != DCP_SUCCESS) {
            dcp_log(LOG_ERROR,"OpenJPEG JPEG2000 conversion failed %s",in_file);
            return DCP_FATAL;
        }
    }
    /* free the image memory */
    odcp_image_free(odcp_image);
    return DCP_SUCCESS;
}

int encode_kakadu(context_t *context, char *in_file, char *out_file) {
    FILE *f = NULL;
    int j,result;
    int max_cs_len;
    int max_comp_size;
    char k_lengths[128];
    char cmd[512];
	FILE *cmdfp = NULL;
	
    /* set the max image and component sizes based on frame_rate */
    max_cs_len = ((float)MAX_DCP_JPEG_BITRATE)/8/context->frame_rate;
    max_comp_size = ((float)max_cs_len)/1.25;

    sprintf(k_lengths,"Creslengths=%d",max_cs_len);
    for (j=0;j<3;j++) {
        sprintf(k_lengths,"%s Creslengths:C%d=%d,%d",k_lengths,j,max_cs_len,max_comp_size);
    }
    sprintf(cmd,"kdu_compress -i %s -o %s Sprofile=CINEMA2K %s -num_threads 1 -quiet -precise",in_file,out_file,k_lengths);
    cmdfp=popen(cmd,"r");
    result=pclose(cmdfp);
    
    if (result) {
            return DCP_FATAL;
    }

    return DCP_SUCCESS;
}

int encode_openjpeg(context_t *context, odcp_image_t *odcp_image, char *out_file) {
    bool result;
    int codestream_length;
    int max_comp_size;
    int max_cs_len;
    opj_cparameters_t parameters;
    opj_cio_t *cio = NULL;
    opj_cinfo_t *cinfo = NULL;
    opj_image_t *opj_image;
    FILE *f = NULL; 


    /* set the max image and component sizes based on frame_rate */
    max_cs_len = ((float)MAX_DCP_JPEG_BITRATE)/8/context->frame_rate;
    if (context->quality) {
        max_cs_len = max_cs_len * ((float)context->quality/100.00);
    }
    max_comp_size = ((float)max_cs_len)/1.25;

    /* set encoding parameters to default values */
    opj_set_default_encoder_parameters(&parameters);

    /* set default cinema parameters */
    set_cinema_encoder_parameters(context, &parameters);

    parameters.cp_comment = (char*)malloc(strlen(OPEN_DCP_NAME)+1);
    sprintf(parameters.cp_comment,"%s", OPEN_DCP_NAME);

    /* adjust cinema enum type */
    if (context->cinema_profile == DCP_CINEMA4K) {
        parameters.cp_cinema = CINEMA4K_24;
    } else {
        parameters.cp_cinema = CINEMA2K_24;
    }

    /* Decide if MCT should be used */
    parameters.tcp_mct = odcp_image->n_components == 3 ? 1 : 0;

    /* set max image */
    parameters.max_comp_size = max_comp_size;
    parameters.tcp_rates[0]= ((float) (odcp_image->n_components * odcp_image->w * odcp_image->h * odcp_image->precision))/
                              (max_cs_len * 8 * odcp_image->dx * odcp_image->dy);

    /* get a J2K compressor handle */
    dcp_log(LOG_DEBUG,"Creating Compressor %s",out_file);
    cinfo = opj_create_compress(CODEC_J2K);

    /* convert opendcp to openjpeg image */
    odcp_to_opj(odcp_image, &opj_image); 

    /* setup the encoder parameters using the current image and user parameters */
    dcp_log(LOG_DEBUG,"Setup J2k Encoder %s",out_file);
    opj_setup_encoder(cinfo, &parameters, opj_image);

    /* open a byte stream for writing */
    /* allocate memory for all tiles */
    dcp_log(LOG_DEBUG,"Opening J2k output stream %s",out_file);
    cio = opj_cio_open((opj_common_ptr)cinfo, NULL, 0);

    dcp_log(LOG_INFO,"Encoding file %s",out_file);
    result = opj_encode(cinfo, cio, opj_image, NULL);
    dcp_log(LOG_DEBUG,"Encoding file %s complete",out_file);

    if (!result) {
        dcp_log(LOG_ERROR,"Unable to encode jpeg2000 file %s",out_file);
        opj_cio_close(cio);
        opj_image_destroy(opj_image);
        opj_destroy_compress(cinfo);
        return DCP_FATAL;
    }
      
    codestream_length = cio_tell(cio);

    f = fopen(out_file, "wb");

    if (!f) {
        dcp_log(LOG_ERROR,"Unable to write jpeg2000 file %s",out_file);
        opj_cio_close(cio);
        opj_image_destroy(opj_image);
        opj_destroy_compress(cinfo);
        return DCP_FATAL;
    }

    fwrite(cio->buffer, 1, codestream_length, f);
    fclose(f);

    /* free openjpeg structure */
    opj_cio_close(cio);
    opj_destroy_compress(cinfo);
    opj_image_destroy(opj_image);

    /* free user parameters structure */
    if(parameters.cp_comment) free(parameters.cp_comment);
    if(parameters.cp_matrice) free(parameters.cp_matrice);

    return DCP_SUCCESS;
}
