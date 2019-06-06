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
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_light.c
#include "tr_globals.h"
#include "ref_import.h"
#include "tr_cvar.h"
#include "tr_light.h"
#include "tr_curve.h"
#include "tr_model.h"
#include "tr_world.h"
#include "srfTriangles_type.h"
#include "srfSurfaceFace_type.h"

#define	DLIGHT_AT_RADIUS		16
// at the edge of a dlight's influence, this amount of light will be added

#define	DLIGHT_MINIMUM_RADIUS	16		
// never calculate a range less than this to prevent huge light numbers


/*
===============
Transforms the origins of an array of dlights.
Used by both the front end (for DlightBmodel) and
the back end (before doing the lighting calculation)
===============
*/
void R_TransformDlights( int count, dlight_t * const pDL, const orientationr_t * const or)
{
	int	i;

	for ( i = 0 ; i < count ; ++i)
    {
        vec3_t temp;
		VectorSubtract( pDL[i].origin, or->origin, temp );
		pDL[i].transformed[0] = DotProduct( temp, or->axis[0] );
		pDL[i].transformed[1] = DotProduct( temp, or->axis[1] );
		pDL[i].transformed[2] = DotProduct( temp, or->axis[2] );
	}
}

/*
=============
R_DlightBmodel

Determine which dynamic lights may effect this bmodel
=============
*/
void R_DlightBmodel( bmodel_t *bmodel )
{
	int			i, j;
	dlight_t	*dl;
	int			mask;
	msurface_t	*surf;

	// transform all the lights
	R_TransformDlights( tr.refdef.num_dlights, tr.refdef.dlights, &tr.or );

	mask = 0;
	for ( i=0 ; i<tr.refdef.num_dlights ; i++ ) {
		dl = &tr.refdef.dlights[i];

		// see if the point is close enough to the bounds to matter
		for ( j = 0 ; j < 3 ; j++ ) {
			if ( dl->transformed[j] - bmodel->bounds[1][j] > dl->radius ) {
				break;
			}
			if ( bmodel->bounds[0][j] - dl->transformed[j] > dl->radius ) {
				break;
			}
		}
		if ( j < 3 ) {
			continue;
		}

		// we need to check this light
		mask |= 1 << i;
	}

	tr.currentEntity->needDlights = (qboolean) (mask != 0);

	// set the dlight bits in all the surfaces
	for ( i = 0 ; i < bmodel->numSurfaces ; i++ ) {
		surf = bmodel->firstSurface + i;

		if ( *surf->data == SF_FACE ) {
			((srfSurfaceFace_t *)surf->data)->dlightBits = mask;
		} else if ( *surf->data == SF_GRID ) {
			((srfGridMesh_t *)surf->data)->dlightBits = mask;
		} else if ( *surf->data == SF_TRIANGLES ) {
			((srfTriangles_t *)surf->data)->dlightBits = mask;
		}
	}
}


/*
=============================================================================

LIGHT SAMPLING

=============================================================================
*/
static void R_SetupEntityLightingGrid( trRefEntity_t * const ent )
{
	vec3_t	lightOrigin;
	int		pos[3];
	int		i, j;
	float	frac[3];



	if ( ent->e.renderfx & RF_LIGHTING_ORIGIN )
    {
		// seperate lightOrigins are needed so an object that is
		// sinking into the ground can still be lit, and so
		// multi-part models can be lit identically
		VectorCopy( ent->e.lightingOrigin, lightOrigin );
	}
    else
    {
		VectorCopy( ent->e.origin, lightOrigin );
	}

	VectorSubtract( lightOrigin, tr.world->lightGridOrigin, lightOrigin );
	
    for( i = 0; i < 3; ++i )
    {
		float v = lightOrigin[i] * tr.world->lightGridInverseSize[i];
		pos[i] = floor( v );
		frac[i] = v - pos[i];
		if ( pos[i] < 0 )
        {
			pos[i] = 0;
		}
        else if ( pos[i] >= tr.world->lightGridBounds[i] - 1 )
        {
			pos[i] = tr.world->lightGridBounds[i] - 1;
		}
	}

	VectorClear( ent->ambientLight );
	VectorClear( ent->directedLight );
	
    // VectorClear( direction );
	float direction[3] = {0.0f, 0.0f, 0.0f};
	assert( tr.world->lightGridData ); // bk010103 - NULL with -nolight maps

	// trilerp the light value
    // 8, 576, 18432
    // 1, 72, 32
    int	gridStep[3] = { 8, 
        8 * tr.world->lightGridBounds[0], 
        8 * tr.world->lightGridBounds[0] * tr.world->lightGridBounds[1]};
    
    // ri.Printf(PRINT_ALL, "gridStep: %d, %d, %d\n", gridStep[0], gridStep[1], gridStep[2]);

	uint8_t * gridData = tr.world->lightGridData + pos[0] * gridStep[0]
		+ pos[1] * gridStep[1] + pos[2] * gridStep[2];

	float totalFactor = 0;
	for ( i = 0 ; i < 8 ; ++i )
    {
		int		lat, lng;
		vec3_t	normal;

        float factor = 1.0f;
		uint8_t* data = gridData;
	
        for ( j = 0 ; j < 3 ; ++j )
        {
			if ( i & (1<<j) )
            {
				factor *= frac[j];
				data += gridStep[j];
			}
            else
            {
				factor *= (1.0f - frac[j]);
			}
		}

		if ( !(data[0]+data[1]+data[2]) ) {
			continue;	// ignore samples in walls
		}
		totalFactor += factor;
		
        ent->ambientLight[0] += factor * data[0];
		ent->ambientLight[1] += factor * data[1];
		ent->ambientLight[2] += factor * data[2];

		ent->directedLight[0] += factor * data[3];
		ent->directedLight[1] += factor * data[4];
		ent->directedLight[2] += factor * data[5];

        lat = data[7];
		lng = data[6];
		lat *= (FUNCTABLE_SIZE/256);
		lng *= (FUNCTABLE_SIZE/256);

		// decode X as cos( lat ) * sin( long )
		// decode Y as sin( lat ) * sin( long )
		// decode Z as cos( long )

		normal[0] = tr.sinTable[(lat+(FUNCTABLE_SIZE/4))&FUNCTABLE_MASK] * tr.sinTable[lng];
		normal[1] = tr.sinTable[lat] * tr.sinTable[lng];
		normal[2] = tr.sinTable[(lng+(FUNCTABLE_SIZE/4))&FUNCTABLE_MASK];

		VectorMA( direction, factor, normal, direction );
	}

	if ( (totalFactor > 0.0f) && (totalFactor < 0.99f) )
    {
		totalFactor = 1.0f / totalFactor;
		VectorScale( ent->ambientLight, totalFactor, ent->ambientLight );
		VectorScale( ent->directedLight, totalFactor, ent->directedLight );
	}

	VectorScale( ent->ambientLight, r_ambientScale->value, ent->ambientLight );
	VectorScale( ent->directedLight, r_directedScale->value, ent->directedLight );

	VectorNormalize2( direction, ent->lightDir );
}


/*
===============
LogLight
===============
*/
static void LogLight( trRefEntity_t *ent ) {
	int	max1, max2;

	if ( !(ent->e.renderfx & RF_FIRST_PERSON ) ) {
		return;
	}

	max1 = ent->ambientLight[0];
	if ( ent->ambientLight[1] > max1 ) {
		max1 = ent->ambientLight[1];
	} else if ( ent->ambientLight[2] > max1 ) {
		max1 = ent->ambientLight[2];
	}

	max2 = ent->directedLight[0];
	if ( ent->directedLight[1] > max2 ) {
		max2 = ent->directedLight[1];
	} else if ( ent->directedLight[2] > max2 ) {
		max2 = ent->directedLight[2];
	}

	ri.Printf( PRINT_ALL, "amb:%i  dir:%i\n", max1, max2 );
}

/*
=================
R_SetupEntityLighting

Calculates all the lighting values that will be used
by the Calc_* functions
=================
*/
void R_SetupEntityLighting( const trRefdef_t *refdef, trRefEntity_t *ent )
{
	int				i;
	dlight_t		*dl;
	float			power;
	vec3_t			dir;
	vec3_t			lightDir;
	vec3_t			lightOrigin;

	// lighting calculations 
	if ( ent->lightingCalculated ) {
		return;
	}
	ent->lightingCalculated = qtrue;

	//
	// trace a sample point down to find ambient light
	//
	if ( ent->e.renderfx & RF_LIGHTING_ORIGIN ) {
		// seperate lightOrigins are needed so an object that is
		// sinking into the ground can still be lit, and so
		// multi-part models can be lit identically
		VectorCopy( ent->e.lightingOrigin, lightOrigin );
	} else {
		VectorCopy( ent->e.origin, lightOrigin );
	}

	// if NOWORLDMODEL, only use dynamic lights (menu system, etc)
	if ( !(refdef->rdflags & RDF_NOWORLDMODEL ) 
		&& tr.world->lightGridData )
    {
		R_SetupEntityLightingGrid( ent );
	}
    else
    {
		ent->ambientLight[0] = ent->ambientLight[1] = 
			ent->ambientLight[2] = tr.identityLight * 150;
		ent->directedLight[0] = ent->directedLight[1] = 
			ent->directedLight[2] = tr.identityLight * 150;
		VectorCopy( tr.sunDirection, ent->lightDir );
	}

	// bonus items and view weapons have a fixed minimum add
	if ( 1 /* ent->e.renderfx & RF_MINLIGHT */ ) {
		// give everything a minimum light add
		ent->ambientLight[0] += tr.identityLight * 32;
		ent->ambientLight[1] += tr.identityLight * 32;
		ent->ambientLight[2] += tr.identityLight * 32;
	}

	//
	// modify the light by dynamic lights
	//
	float d = VectorLength( ent->directedLight );
	VectorScale( ent->lightDir, d, lightDir );

	for ( i = 0 ; i < refdef->num_dlights ; ++i )
    {
		dl = &refdef->dlights[i];
		VectorSubtract( dl->origin, lightOrigin, dir );
		d = VectorNormalize( dir );

		power = DLIGHT_AT_RADIUS * ( dl->radius * dl->radius );
		if ( d < DLIGHT_MINIMUM_RADIUS ) {
			d = DLIGHT_MINIMUM_RADIUS;
		}
		d = power / ( d * d );

		VectorMA( ent->directedLight, d, dl->color, ent->directedLight );
		VectorMA( lightDir, d, dir, lightDir );
	}

	// clamp ambient
	for ( i = 0 ; i < 3 ; ++i )
    {
		if ( ent->ambientLight[i] > tr.identityLightByte )
        {
			ent->ambientLight[i] = tr.identityLightByte;
		}
	}

	if ( r_debugLight->integer ) {
		LogLight( ent );
	}

	// save out the byte packet version
	ent->ambientLightRGBA[0] = (unsigned char)( ent->ambientLight[0] );
	ent->ambientLightRGBA[1] = (unsigned char)( ent->ambientLight[1] );
	ent->ambientLightRGBA[2] = (unsigned char)( ent->ambientLight[2] );
	ent->ambientLightRGBA[3] = 0xff;
	
	// transform the direction to local space
	VectorNormalize( lightDir );
	ent->lightDir[0] = DotProduct( lightDir, ent->e.axis[0] );
	ent->lightDir[1] = DotProduct( lightDir, ent->e.axis[1] );
	ent->lightDir[2] = DotProduct( lightDir, ent->e.axis[2] );
}

// R_LightForPoint
int RE_LightForPoint( vec3_t point, vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir )
{
	
	// bk010103 - this segfaults with -nolight maps
	if ( tr.world->lightGridData == NULL ) {
	  return qfalse;
    }
    
    trRefEntity_t ent;
	memset(&ent, 0, sizeof(ent));
	
    VectorCopy( point, ent.e.origin );

    R_SetupEntityLightingGrid( &ent );
	VectorCopy(ent.ambientLight, ambientLight);
	VectorCopy(ent.directedLight, directedLight);
	VectorCopy(ent.lightDir, lightDir);

	return qtrue;
}
