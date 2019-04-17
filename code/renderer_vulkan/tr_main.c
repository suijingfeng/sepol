/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_main.c -- main control flow for each frame

#include "tr_local.h"
#include "tr_globals.h"
#include "tr_cvar.h"
#include "tr_shader.h"

#include "vk_shade_geometry.h"
#include "vk_instance.h"
#include "vk_image.h"
#include "../renderercommon/matrix_multiplication.h"
#include "../renderercommon/ref_import.h"

#include "R_PrintMat.h"

#include "R_DebugGraphics.h"
#include "R_Portal.h"
#include "tr_cmds.h"


static void R_WorldVectorToLocal (const vec3_t world, const float R[3][3], vec3_t local)
{
	local[0] = DotProduct(world, R[0]);
	local[1] = DotProduct(world, R[1]);
	local[2] = DotProduct(world, R[2]);
}


static void R_WorldPointToLocal (const vec3_t world, const orientationr_t * const pRT, vec3_t local)
{
    vec3_t delta;

    VectorSubtract( world, pRT->origin, delta );

	local[0] = DotProduct(delta, pRT->axis[0]);
	local[1] = DotProduct(delta, pRT->axis[1]);
	local[2] = DotProduct(delta, pRT->axis[2]);
}



/*
=================
R_RotateForEntity

Generates an orientation for an entity and viewParms
Does NOT produce any GL calls
Called by both the front end and the back end

typedef struct {
	float		modelMatrix[16] QALIGN(16);
	float		axis[3][3];		// orientation in world
    float		origin[3];		// in world coordinates
	float		viewOrigin[3];	// viewParms->or.origin in local coordinates
} orientationr_t;

=================
*/
void R_RotateForEntity(const trRefEntity_t* const ent, const viewParms_t* const viewParms, orientationr_t* const or)
{

	if ( ent->e.reType != RT_MODEL )
    {
		*or = viewParms->world;
		return;
	}

	//VectorCopy( ent->e.origin, or->origin );
	//VectorCopy( ent->e.axis[0], or->axis[0] );
	//VectorCopy( ent->e.axis[1], or->axis[1] );
	//VectorCopy( ent->e.axis[2], or->axis[2] );
    memcpy(or->origin, ent->e.origin, 12);
    memcpy(or->axis, ent->e.axis, 36);

	float glMatrix[16] QALIGN(16);

	glMatrix[0] = or->axis[0][0];
	glMatrix[1] = or->axis[0][1];
	glMatrix[2] = or->axis[0][2];
	glMatrix[3] = 0;

    glMatrix[4] = or->axis[1][0];
	glMatrix[5] = or->axis[1][1];
	glMatrix[6] = or->axis[1][2];
	glMatrix[7] = 0;
    
    glMatrix[8] = or->axis[2][0];
	glMatrix[9] = or->axis[2][1];
	glMatrix[10] = or->axis[2][2];
	glMatrix[11] = 0;

	glMatrix[12] = or->origin[0];
	glMatrix[13] = or->origin[1];
	glMatrix[14] = or->origin[2];
	glMatrix[15] = 1;

	MatrixMultiply4x4_SSE( glMatrix, viewParms->world.modelMatrix, or->modelMatrix );

	// calculate the viewer origin in the model's space
	// needed for fog, specular, and environment mapping
    
    R_WorldPointToLocal(viewParms->or.origin, or, or->viewOrigin);
    
    if ( ent->e.nonNormalizedAxes )
    {
         if ( ent->e.nonNormalizedAxes )
        {
            const float * v = ent->e.axis[0];
            float axisLength = v[0] * v[0] + v[0] * v[0] + v[2] * v[2]; 
            if ( axisLength ) {
                axisLength = 1.0f / sqrtf(axisLength);
            }

            or->viewOrigin[0] *= axisLength;
            or->viewOrigin[1] *= axisLength;
            or->viewOrigin[2] *= axisLength;
        }
    }
/*  
    vec3_t delta;

	VectorSubtract( viewParms->or.origin, or->origin, delta );

    R_WorldVectorToLocal(delta, or->axis, or->viewOrigin);
    
	// compensate for scale in the axes if necessary
    float axisLength = 1.0f;
	if ( ent->e.nonNormalizedAxes )
    {
		axisLength = VectorLength( ent->e.axis[0] );
		if ( axisLength ) {
			axisLength = 1.0f / axisLength;
		}
	}

	or->viewOrigin[0] = DotProduct( delta, or->axis[0] ) * axisLength;
	or->viewOrigin[1] = DotProduct( delta, or->axis[1] ) * axisLength;
	or->viewOrigin[2] = DotProduct( delta, or->axis[2] ) * axisLength;

*/
    // printMat1x3f("viewOrigin", or->viewOrigin);
    // printMat4x4f("modelMatrix", or->modelMatrix);

}

/*
=================
typedef struct {
	float		modelMatrix[16] QALIGN(16);
	float		axis[3][3];		// orientation in world
    float		origin[3];			// in world coordinates
	float		viewOrigin[3];		// viewParms->or.origin in local coordinates
} orientationr_t;


Sets up the modelview matrix for a given viewParm

IN: tr.viewParms
OUT: tr.or
=================
*/
void R_RotateForViewer ( viewParms_t * const pViewParams, orientationr_t * const pEntityPose) 
{
    //const viewParms_t * const pViewParams = &tr.viewParms;
    // for current entity
    // orientationr_t * const pEntityPose = &tr.or;
    
    const static float s_flipMatrix[16] QALIGN(16) = {
        // convert from our coordinate system (looking down X)
        // to OpenGL's coordinate system (looking down -Z)
        0, 0, -1, 0,
        -1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 0, 1
    };
    

	float o0, o1, o2;

    pEntityPose->origin[0] = pEntityPose->origin[1] = pEntityPose->origin[2] = 0;
    Mat3x3Identity(pEntityPose->axis);


    // transform by the camera placement
	// VectorCopy( tr.viewParms.or.origin, tr.or.viewOrigin );
	// VectorCopy( tr.viewParms.or.origin, origin );

    pEntityPose->viewOrigin[0] = o0 = pViewParams->or.origin[0];
    pEntityPose->viewOrigin[1] = o1 = pViewParams->or.origin[1];
    pEntityPose->viewOrigin[2] = o2 = pViewParams->or.origin[2];

    
    float viewerMatrix[16] QALIGN(16);
	viewerMatrix[0] = pViewParams->or.axis[0][0];
	viewerMatrix[1] = pViewParams->or.axis[1][0];
	viewerMatrix[2] = pViewParams->or.axis[2][0];
	viewerMatrix[3] = 0;

	viewerMatrix[4] = pViewParams->or.axis[0][1];
	viewerMatrix[5] = pViewParams->or.axis[1][1];
	viewerMatrix[6] = pViewParams->or.axis[2][1];
	viewerMatrix[7] = 0;

	viewerMatrix[8] = pViewParams->or.axis[0][2];
	viewerMatrix[9] = pViewParams->or.axis[1][2];
	viewerMatrix[10] = pViewParams->or.axis[2][2];
	viewerMatrix[11] = 0;

	viewerMatrix[12] = - o0 * viewerMatrix[0] - o1 * viewerMatrix[4] - o2 * viewerMatrix[8];
	viewerMatrix[13] = - o0 * viewerMatrix[1] - o1 * viewerMatrix[5] - o2 * viewerMatrix[9];
	viewerMatrix[14] = - o0 * viewerMatrix[2] - o1 * viewerMatrix[6] - o2 * viewerMatrix[10];
	viewerMatrix[15] = 1;

	// convert from our coordinate system (looking down X)
	// to OpenGL's coordinate system (looking down -Z)
	MatrixMultiply4x4_SSE( viewerMatrix, s_flipMatrix, pEntityPose->modelMatrix );

	pViewParams->world = *pEntityPose;
}


/*
=================
Setup that culling frustum planes for the current view
=================
*/
static void R_SetupFrustum (viewParms_t * const pViewParams)
{
	
    {
        float ang = pViewParams->fovX * (float)(M_PI / 360.0f);
        float xs = sin( ang );
        float xc = cos( ang );

        float temp1[3];
        float temp2[3];

        VectorScale( pViewParams->or.axis[0], xs, temp1 );
        VectorScale( pViewParams->or.axis[1], xc, temp2);

        VectorAdd(temp1, temp2, pViewParams->frustum[0].normal);
		pViewParams->frustum[0].dist = DotProduct (pViewParams->or.origin, pViewParams->frustum[0].normal);
        pViewParams->frustum[0].type = PLANE_NON_AXIAL;

        VectorSubtract(temp1, temp2, pViewParams->frustum[1].normal);
		pViewParams->frustum[1].dist = DotProduct (pViewParams->or.origin, pViewParams->frustum[1].normal);
        pViewParams->frustum[1].type = PLANE_NON_AXIAL;
    }

   
    {
        float ang = pViewParams->fovY * (float)(M_PI / 360.0f);
        float xs = sin( ang );
        float xc = cos( ang );
        float temp1[3];
        float temp2[3];

        VectorScale( pViewParams->or.axis[0], xs, temp1);
        VectorScale( pViewParams->or.axis[2], xc, temp2);

        VectorAdd(temp1, temp2, pViewParams->frustum[2].normal);
		pViewParams->frustum[2].dist = DotProduct (pViewParams->or.origin, pViewParams->frustum[2].normal);
        pViewParams->frustum[2].type = PLANE_NON_AXIAL;

        VectorSubtract(temp1, temp2, pViewParams->frustum[3].normal);
		pViewParams->frustum[3].dist = DotProduct (pViewParams->or.origin, pViewParams->frustum[3].normal);
		pViewParams->frustum[3].type = PLANE_NON_AXIAL;
    }


    uint32_t i = 0;
	for (i=0; i < 4; i++)
    {
		// pViewParams->frustum[i].type = PLANE_NON_AXIAL;
		// pViewParams->frustum[i].dist = DotProduct (pViewParams->or.origin, pViewParams->frustum[i].normal);
		SetPlaneSignbits( &pViewParams->frustum[i] );
	}
}


/*
=================
R_SpriteFogNum

See if a sprite is inside a fog volume
=================
*/
int R_SpriteFogNum( trRefEntity_t *ent ) {
	int				i, j;
	fog_t			*fog;

	if ( tr.refdef.rd.rdflags & RDF_NOWORLDMODEL ) {
		return 0;
	}

	for ( i = 1 ; i < tr.world->numfogs ; i++ ) {
		fog = &tr.world->fogs[i];
		for ( j = 0 ; j < 3 ; j++ ) {
			if ( ent->e.origin[j] - ent->e.radius >= fog->bounds[1][j] ) {
				break;
			}
			if ( ent->e.origin[j] + ent->e.radius <= fog->bounds[0][j] ) {
				break;
			}
		}
		if ( j == 3 ) {
			return i;
		}
	}

	return 0;
}

/*
==========================================================================================

DRAWSURF SORTING

==========================================================================================
*/

/*
=================
qsort replacement

=================
*/
void SWAP_DRAW_SURF(void* a, void* b)
{
    char buf[sizeof(drawSurf_t)];
    memcpy(buf, a, sizeof(drawSurf_t));
    memcpy(a, b, sizeof(drawSurf_t));
    memcpy(b, buf, sizeof(drawSurf_t));
}


/* this parameter defines the cutoff between using quick sort and
   insertion sort for arrays; arrays with lengths shorter or equal to the
   below value use insertion sort */

#define CUTOFF 8            /* testing shows that this is good value */

static void shortsort( drawSurf_t *lo, drawSurf_t *hi )
{
    drawSurf_t	*p, *max;

    while (hi > lo) {
        max = lo;
        for (p = lo + 1; p <= hi; p++ ) {
            if ( p->sort > max->sort ) {
                max = p;
            }
        }
        SWAP_DRAW_SURF(max, hi);
        hi--;
    }
}


/* sort the array between lo and hi (inclusive)
FIXME: this was lifted and modified from the microsoft lib source...
 */

void qsortFast (
    void *base,
    unsigned num,
    unsigned width
    )
{
    char *lo, *hi;              /* ends of sub-array currently sorting */
    char *mid;                  /* points to middle of subarray */
    char *loguy, *higuy;        /* traveling pointers for partition step */
    unsigned size;              /* size of the sub-array */
    char *lostk[30], *histk[30];
    int stkptr;                 /* stack for saving sub-array to be processed */

    /* Note: the number of stack entries required is no more than
       1 + log2(size), so 30 is sufficient for any array */

    if (num < 2 || width == 0)
        return;                 /* nothing to do */

    stkptr = 0;                 /* initialize stack */

    lo = (char*) base;
    hi = (char *)base + width * (num-1);        /* initialize limits */

    /* this entry point is for pseudo-recursion calling: setting
       lo and hi and jumping to here is like recursion, but stkptr is
       prserved, locals aren't, so we preserve stuff on the stack */
recurse:

    size = (hi - lo) / width + 1;        /* number of el's to sort */

    /* below a certain size, it is faster to use a O(n^2) sorting method */
    if (size <= CUTOFF) {
         shortsort((drawSurf_t *)lo, (drawSurf_t *)hi);
    }
    else {
        /* First we pick a partititioning element.  The efficiency of the
           algorithm demands that we find one that is approximately the
           median of the values, but also that we select one fast.  Using
           the first one produces bad performace if the array is already
           sorted, so we use the middle one, which would require a very
           wierdly arranged array for worst case performance.  Testing shows
           that a median-of-three algorithm does not, in general, increase
           performance. */

        mid = lo + (size / 2) * width;      /* find middle element */
        SWAP_DRAW_SURF(mid, lo);               /* swap it to beginning of array */

        /* We now wish to partition the array into three pieces, one
           consisiting of elements <= partition element, one of elements
           equal to the parition element, and one of element >= to it.  This
           is done below; comments indicate conditions established at every
           step. */

        loguy = lo;
        higuy = hi + width;

        /* Note that higuy decreases and loguy increases on every iteration,
           so loop must terminate. */
        for (;;) {
            /* lo <= loguy < hi, lo < higuy <= hi + 1,
               A[i] <= A[lo] for lo <= i <= loguy,
               A[i] >= A[lo] for higuy <= i <= hi */

            do  {
                loguy += width;
            } while (loguy <= hi &&  
				( ((drawSurf_t *)loguy)->sort <= ((drawSurf_t *)lo)->sort ) );

            /* lo < loguy <= hi+1, A[i] <= A[lo] for lo <= i < loguy,
               either loguy > hi or A[loguy] > A[lo] */

            do  {
                higuy -= width;
            } while (higuy > lo && 
				( ((drawSurf_t *)higuy)->sort >= ((drawSurf_t *)lo)->sort ) );

            /* lo-1 <= higuy <= hi, A[i] >= A[lo] for higuy < i <= hi,
               either higuy <= lo or A[higuy] < A[lo] */

            if (higuy < loguy)
                break;

            /* if loguy > hi or higuy <= lo, then we would have exited, so
               A[loguy] > A[lo], A[higuy] < A[lo],
               loguy < hi, highy > lo */

            SWAP_DRAW_SURF(loguy, higuy);

            /* A[loguy] < A[lo], A[higuy] > A[lo]; so condition at top
               of loop is re-established */
        }

        /*     A[i] >= A[lo] for higuy < i <= hi,
               A[i] <= A[lo] for lo <= i < loguy,
               higuy < loguy, lo <= higuy <= hi
           implying:
               A[i] >= A[lo] for loguy <= i <= hi,
               A[i] <= A[lo] for lo <= i <= higuy,
               A[i] = A[lo] for higuy < i < loguy */

        SWAP_DRAW_SURF(lo, higuy);     /* put partition element in place */

        /* OK, now we have the following:
              A[i] >= A[higuy] for loguy <= i <= hi,
              A[i] <= A[higuy] for lo <= i < higuy
              A[i] = A[lo] for higuy <= i < loguy    */

        /* We've finished the partition, now we want to sort the subarrays
           [lo, higuy-1] and [loguy, hi].
           We do the smaller one first to minimize stack usage.
           We only sort arrays of length 2 or more.*/

        if ( higuy - 1 - lo >= hi - loguy ) {
            if (lo + width < higuy) {
                lostk[stkptr] = lo;
                histk[stkptr] = higuy - width;
                ++stkptr;
            }                           /* save big recursion for later */

            if (loguy < hi) {
                lo = loguy;
                goto recurse;           /* do small recursion */
            }
        }
        else {
            if (loguy < hi) {
                lostk[stkptr] = loguy;
                histk[stkptr] = hi;
                ++stkptr;               /* save big recursion for later */
            }

            if (lo + width < higuy) {
                hi = higuy - width;
                goto recurse;           /* do small recursion */
            }
        }
    }

    /* We have sorted the array, except for any pending sorts on the stack.
       Check if there are any, and do them. */

    --stkptr;
    if (stkptr >= 0) {
        lo = lostk[stkptr];
        hi = histk[stkptr];
        goto recurse;           /* pop subarray from stack */
    }
    else
        return;                 /* all subarrays done */
}


//==========================================================================================
/*
=================
R_AddDrawSurf
=================
*/
void R_AddDrawSurf( surfaceType_t *surface, shader_t *shader, int fogIndex, int dlightMap )
{
	// instead of checking for overflow, we just mask the index so it wraps around
	int index = tr.refdef.numDrawSurfs & DRAWSURF_MASK;
	// the sort data is packed into a single 32 bit value so it can be
	// compared quickly during the qsorting process
	tr.refdef.drawSurfs[index].sort = (shader->sortedIndex << QSORT_SHADERNUM_SHIFT) 
		| tr.shiftedEntityNum | ( fogIndex << QSORT_FOGNUM_SHIFT ) | (int)dlightMap;
	tr.refdef.drawSurfs[index].surface = surface;
	tr.refdef.numDrawSurfs++;
}

/*
=================
R_DecomposeSort
=================
*/
void R_DecomposeSort( unsigned sort, int *entityNum, shader_t **shader, 
					 int *fogNum, int *dlightMap ) {
	*fogNum = ( sort >> QSORT_FOGNUM_SHIFT ) & 31;
	*shader = tr.sortedShaders[ ( sort >> QSORT_SHADERNUM_SHIFT ) & (MAX_SHADERS-1) ];
	*entityNum = ( sort >> QSORT_ENTITYNUM_SHIFT ) & 1023;
	*dlightMap = sort & 3;
}


static void R_SortDrawSurfs( drawSurf_t *drawSurfs, int numDrawSurfs )
{
	shader_t		*shader;
	int				fogNum;
	int				entityNum;
	int				dlighted;
	int				i;

	// it is possible for some views to not have any surfaces
	if ( numDrawSurfs < 1 ) {
		// we still need to add it for hyperspace cases
		R_AddDrawSurfCmd( drawSurfs, numDrawSurfs );
		return;
	}

	// if we overflowed MAX_DRAWSURFS, the drawsurfs
	// wrapped around in the buffer and we will be missing
	// the first surfaces, not the last ones
	if ( numDrawSurfs > MAX_DRAWSURFS ) {
		numDrawSurfs = MAX_DRAWSURFS;
        ri.Printf(PRINT_WARNING, " numDrawSurfs overflowed. \n");

	}

	// sort the drawsurfs by sort type, then orientation, then shader
	qsortFast (drawSurfs, numDrawSurfs, sizeof(drawSurf_t) );

	// check for any pass through drawing, which
	// may cause another view to be rendered first
	for ( i = 0 ; i < numDrawSurfs ; i++ )
    {
		R_DecomposeSort( (drawSurfs+i)->sort, &entityNum, &shader, &fogNum, &dlighted );

		if ( shader->sort > SS_PORTAL ) {
			break;
		}

		// no shader should ever have this sort type
		if ( shader->sort == SS_BAD ) {
			ri.Error (ERR_DROP, "Shader '%s'with sort == SS_BAD", shader->name );
		}

		// if the mirror was completely clipped away, we may need to check another surface
		if ( R_MirrorViewBySurface( (drawSurfs+i), entityNum) ) {
			// this is a debug option to see exactly what is being mirrored
			if ( r_portalOnly->integer ) {
				return;
			}
			break;		// only one mirror view at a time
		}
	}

	R_AddDrawSurfCmd( drawSurfs, numDrawSurfs );
}



void R_AddEntitySurfaces (viewParms_t * const pViewParam)
{
    // entities that will have procedurally generated surfaces will just
    // point at this for their sorting surface
    static surfaceType_t	entitySurface = SF_ENTITY;
	if ( !r_drawentities->integer ) {
		return;
	}

	for ( tr.currentEntityNum = 0; 
	      tr.currentEntityNum < tr.refdef.num_entities; 
		  tr.currentEntityNum++ )
    {
		shader_t* shader;

        trRefEntity_t* ent = tr.currentEntity = &tr.refdef.entities[tr.currentEntityNum];

		ent->needDlights = qfalse;

		// preshift the value we are going to OR into the drawsurf sort
		tr.shiftedEntityNum = tr.currentEntityNum << QSORT_ENTITYNUM_SHIFT;

		//
		// the weapon model must be handled special --
		// we don't want the hacked weapon position showing in 
		// mirrors, because the true body position will already be drawn
		//
		if ( (ent->e.renderfx & RF_FIRST_PERSON) && pViewParam->isPortal)
        {
			continue;
		}

		// simple generated models, like sprites and beams, are not culled
		switch ( ent->e.reType )
        {
		case RT_PORTALSURFACE:
			break;		// don't draw anything
		case RT_SPRITE:
		case RT_BEAM:
		case RT_LIGHTNING:
		case RT_RAIL_CORE:
		case RT_RAIL_RINGS:
			// self blood sprites, talk balloons, etc should not be drawn in the primary
			// view.  We can't just do this check for all entities, because md3
			// entities may still want to cast shadows from them
			if ( (ent->e.renderfx & RF_THIRD_PERSON) && !pViewParam->isPortal)
            {
				continue;
			}
			shader = R_GetShaderByHandle( ent->e.customShader );
			R_AddDrawSurf( &entitySurface, shader, R_SpriteFogNum( ent ), 0 );
			break;

		case RT_MODEL:
			// we must set up parts of tr.or for model culling
			R_RotateForEntity( ent, pViewParam, &tr.or );

			tr.currentModel = R_GetModelByHandle( ent->e.hModel );
			if (!tr.currentModel)
            {
				R_AddDrawSurf( &entitySurface, tr.defaultShader, 0, 0 );
			}
            else
            {
				switch ( tr.currentModel->type )
                {
				case MOD_MESH:
					R_AddMD3Surfaces( ent );
					break;
				case MOD_MDR:
					R_MDRAddAnimSurfaces( ent );
					break;
				case MOD_IQM:
					R_AddIQMSurfaces( ent );
				case MOD_BRUSH:
					R_AddBrushModelSurfaces( ent );
					break;
				case MOD_BAD:		// null model axis
					if ( (ent->e.renderfx & RF_THIRD_PERSON) && !pViewParam->isPortal) {
						break;
					}
					shader = R_GetShaderByHandle( ent->e.customShader );
					R_AddDrawSurf( &entitySurface, tr.defaultShader, 0, 0 );
					break;
				default:
					ri.Error( ERR_DROP, "Add entity surfaces: Bad modeltype" );
					break;
				}
			}
			break;
		default:
			ri.Error( ERR_DROP, "Add entity surfaces: Bad reType" );
		}
	}
}


static void R_SetupProjection( viewParms_t * const pViewParams)
{
	float zFar;

	// set the projection matrix with the minimum zfar
	// now that we have the world bounded
	// this needs to be done before entities are added, 
    // because they use the projection matrix for lod calculation

	// dynamically compute far clip plane distance
	// if not rendering the world (icons, menus, etc), set a 2k far clip plane

	if ( tr.refdef.rd.rdflags & RDF_NOWORLDMODEL )
    {
		pViewParams->zFar = zFar = 2048.0f;
	}
    else
    {
        float o[3];

        o[0] = pViewParams->or.origin[0];
        o[1] = pViewParams->or.origin[1];
        o[2] = pViewParams->or.origin[2];

        float farthestCornerDistance = 0;
        uint32_t i;

        // set far clipping planes dynamically
        for ( i = 0; i < 8; i++ )
        {
            float v[3];
     
            v[0] = ((i & 1) ? pViewParams->visBounds[0][0] : pViewParams->visBounds[1][0]) - o[0];
            v[1] = ((i & 2) ? pViewParams->visBounds[0][1] : pViewParams->visBounds[1][1]) - o[1];
            v[2] = ((i & 4) ? pViewParams->visBounds[0][2] : pViewParams->visBounds[1][2]) - o[0];

            float distance = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
            
            if( distance > farthestCornerDistance )
            {
                farthestCornerDistance = distance;
            }
        }
        
        pViewParams->zFar = zFar = sqrtf(farthestCornerDistance);
    }
	
	// set up projection matrix
	// update q3's proj matrix (opengl) to vulkan conventions: z - [0, 1] instead of [-1, 1] and invert y direction
    
    // Vulkan clip space has inverted Y and half Z.	
    float zNear	= r_znear->value;
	float p10 = -zFar / (zFar - zNear);

    float py = tan(pViewParams->fovY * (M_PI / 360.0f));
    float px = tan(pViewParams->fovX * (M_PI / 360.0f));

	pViewParams->projectionMatrix[0] = 1.0f / px;
	pViewParams->projectionMatrix[1] = 0;
	pViewParams->projectionMatrix[2] = 0;
	pViewParams->projectionMatrix[3] = 0;
    
    pViewParams->projectionMatrix[4] = 0;
	pViewParams->projectionMatrix[5] = -1.0f / py;
	pViewParams->projectionMatrix[6] = 0;
	pViewParams->projectionMatrix[7] = 0;

    pViewParams->projectionMatrix[8] = 0;	// normally 0
	pViewParams->projectionMatrix[9] =  0;
	pViewParams->projectionMatrix[10] = p10;
	pViewParams->projectionMatrix[11] = -1.0f;

    pViewParams->projectionMatrix[12] = 0;
	pViewParams->projectionMatrix[13] = 0;
	pViewParams->projectionMatrix[14] = zNear * p10;
	pViewParams->projectionMatrix[15] = 0;
}



/*
================
R_RenderView

A view may be either the actual camera view,
or a mirror / remote location
================
*/
void R_RenderView (viewParms_t *parms)
{
	int	firstDrawSurf;

	tr.viewCount++;

	tr.viewParms = *parms;

	firstDrawSurf = tr.refdef.numDrawSurfs;

	tr.viewCount++;

	// set viewParms.world
	R_RotateForViewer (&tr.viewParms, &tr.or);
    // Setup that culling frustum planes for the current view
	R_SetupFrustum (&tr.viewParms);

	R_AddWorldSurfaces ();

	R_AddPolygonSurfaces();

	// set the projection matrix with the minimum zfar
	// now that we have the world bounded
	// this needs to be done before entities are
	// added, because they use the projection matrix for LOD calculation
	R_SetupProjection (&tr.viewParms);

	R_AddEntitySurfaces (&tr.viewParms);

	R_SortDrawSurfs( tr.refdef.drawSurfs + firstDrawSurf, tr.refdef.numDrawSurfs - firstDrawSurf );

    if ( r_debugSurface->integer )
    {
        // draw main system development information (surface outlines, etc)
		R_DebugGraphics();
	}
}
