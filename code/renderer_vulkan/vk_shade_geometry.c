#include "vk_instance.h"
#include "tr_globals.h"
#include "tr_cvar.h"
#include "tr_backend.h"
#include "tr_light.h"
#include "tr_shader.h"
#include "tr_shade.h"

#include "vk_shade_geometry.h"
#include "vk_image.h"
#include "vk_pipelines.h"

#include "ref_import.h" 
#include "matrix_multiplication.h"
#include "R_PortalPlane.h"



#define VERTEX_CHUNK_SIZE   (768 * 1024)
#define INDEX_BUFFER_SIZE   (2 * 1024 * 1024)

#define XYZ_SIZE            (4 * VERTEX_CHUNK_SIZE)
#define COLOR_SIZE          (1 * VERTEX_CHUNK_SIZE)
#define ST0_SIZE            (2 * VERTEX_CHUNK_SIZE)
#define ST1_SIZE            (2 * VERTEX_CHUNK_SIZE)

#define XYZ_OFFSET          0
#define COLOR_OFFSET        (XYZ_OFFSET + XYZ_SIZE)
#define ST0_OFFSET          (COLOR_OFFSET + COLOR_SIZE)
#define ST1_OFFSET          (ST0_OFFSET + ST0_SIZE)

struct ShadingData_t
{
    // Buffers represent linear arrays of data which are used for various purposes
    // by binding them to a graphics or compute pipeline via descriptor sets or 
    // via certain commands,  or by directly specifying them as parameters to 
    // certain commands. Buffers are represented by VkBuffer handles:
	VkBuffer vertex_buffer;
	unsigned char* vertex_buffer_ptr ; // pointer to mapped vertex buffer
	uint32_t xyz_elements;
	uint32_t color_st_elements;

	VkBuffer index_buffer;
	unsigned char* index_buffer_ptr; // pointer to mapped index buffer
	uint32_t index_buffer_offset;

	// host visible memory that holds both vertex and index data
	VkDeviceMemory vertex_buffer_memory;
	VkDeviceMemory index_buffer_memory;
    VkDescriptorSet curDescriptorSets[2];

    // This flag is used to decide whether framebuffer's depth attachment should be cleared
    // with vmCmdClearAttachment (dirty_depth_attachment == true), or it have just been
    // cleared by render pass instance clear op (dirty_depth_attachment == false).

    VkBool32 s_depth_attachment_dirty;
};

struct ShadingData_t shadingDat;


VkBuffer vk_getIndexBuffer(void)
{
    return shadingDat.index_buffer;
}

VkBuffer vk_getVertexBuffer(void)
{
    return shadingDat.vertex_buffer;
}

static float s_modelview_matrix[16] QALIGN(16);


static float s_ProjectMat2d[16] QALIGN(16);


void R_Set2dProjectMatrix(float width, float height)
{
    s_ProjectMat2d[0] = 2.0f / width; 
    s_ProjectMat2d[1] = 0.0f; 
    s_ProjectMat2d[2] = 0.0f;
    s_ProjectMat2d[3] = 0.0f;

    s_ProjectMat2d[4] = 0.0f; 
    s_ProjectMat2d[5] = 2.0f / height; 
    s_ProjectMat2d[6] = 0.0f;
    s_ProjectMat2d[7] = 0.0f;

    s_ProjectMat2d[8] = 0.0f; 
    s_ProjectMat2d[9] = 0.0f; 
    s_ProjectMat2d[10] = 1.0f; 
    s_ProjectMat2d[11] = 0.0f;

    s_ProjectMat2d[12] = -1.0f; 
    s_ProjectMat2d[13] = -1.0f; 
    s_ProjectMat2d[14] = 0.0f;
    s_ProjectMat2d[15] = 1.0f;
}


void set_modelview_matrix(const float mv[16])
{
    memcpy(s_modelview_matrix, mv, 64);
}


const float * getptr_modelview_matrix()
{
    return s_modelview_matrix;
}


// Vulkan memory is broken up into two categories, host memory and device memory.
// Host memory is memory needed by the Vulkan implementation for 
// non-device-visible storage. Allocations returned by vkAllocateMemory
// are guaranteed to meet any alignment requirement of the implementation
//
// Host access to buffer must be externally synchronized

void vk_createVertexBuffer(void)
{
    ri.Printf(PRINT_ALL, " Create vertex buffer: shadingDat.vertex_buffer \n");

    vk_createBufferResource( XYZ_SIZE + COLOR_SIZE + ST0_SIZE + ST1_SIZE, 
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            &shadingDat.vertex_buffer,
            &shadingDat.vertex_buffer_memory );

    void* data;
    VK_CHECK(qvkMapMemory(vk.device, shadingDat.vertex_buffer_memory, 0, VK_WHOLE_SIZE, 0, &data));
    shadingDat.vertex_buffer_ptr = (unsigned char*)data;
}



void vk_createIndexBuffer(void)
{
    ri.Printf(PRINT_ALL, " Create index buffer: shadingDat.index_buffer \n");

    vk_createBufferResource( INDEX_BUFFER_SIZE, 
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            &shadingDat.index_buffer, &shadingDat.index_buffer_memory);

    void* data;
    VK_CHECK(qvkMapMemory(vk.device, shadingDat.index_buffer_memory, 0, VK_WHOLE_SIZE, 0, &data));
    shadingDat.index_buffer_ptr = (unsigned char*)data;
}


// Descriptors and Descriptor Sets
// A descriptor is a special opaque shader variable that shaders use to access buffer 
// and image resources in an indirect fashion. It can be thought of as a "pointer" to
// a resource.  The Vulkan API allows these variables to be changed between draw
// operations so that the shaders can access different resources for each draw.

// A descriptor set is called a "set" because it can refer to an array of homogenous
// resources that can be described with the same layout binding. one possible way to
// use multiple descriptors is to construct a descriptor set with two descriptors, 
// with each descriptor referencing a separate texture. Both textures are therefore
// available during a draw. A command in a command buffer could then select the texture
// to use by specifying the index of the desired texture. To describe a descriptor set,
// you use a descriptor set layout.

// Descriptor sets corresponding to bound texture images.

// outside of TR since it shouldn't be cleared during ref re-init
// the renderer front end should never modify glstate_t
//typedef struct {



void updateCurDescriptor( VkDescriptorSet curDesSet, uint32_t tmu)
{
    shadingDat.curDescriptorSets[tmu] = curDesSet;
}



void vk_shade_geometry(VkPipeline pipeline, VkBool32 multitexture, VkBool32 is2D,
        enum Vk_Depth_Range depRg, VkBool32 indexed)
{
    // configure vertex data stream
    VkBuffer bufs[3] = { shadingDat.vertex_buffer, shadingDat.vertex_buffer, shadingDat.vertex_buffer };
    VkDeviceSize offs[3] = {
        COLOR_OFFSET + shadingDat.color_st_elements * 4, // sizeof(color4ub_t)
        ST0_OFFSET   + shadingDat.color_st_elements * sizeof(vec2_t),
        ST1_OFFSET   + shadingDat.color_st_elements * sizeof(vec2_t)
    };

    // color, sizeof(color4ub_t)
    if ((shadingDat.color_st_elements + tess.numVertexes) * 4 > COLOR_SIZE)
        ri.Error(ERR_DROP, "vulkan: vertex buffer overflow (color) %d \n", 
                (shadingDat.color_st_elements + tess.numVertexes) * 4);

    unsigned char* dst_color = shadingDat.vertex_buffer_ptr + offs[0];
    memcpy(dst_color, tess.svars.colors, tess.numVertexes * 4);
    // st0

    unsigned char* dst_st0 = shadingDat.vertex_buffer_ptr + offs[1];
    memcpy(dst_st0, tess.svars.texcoords[0], tess.numVertexes * sizeof(vec2_t));

    // st1
    if (multitexture)
    {
        unsigned char* dst = shadingDat.vertex_buffer_ptr + offs[2];
        memcpy(dst, tess.svars.texcoords[1], tess.numVertexes * sizeof(vec2_t));
    }

    NO_CHECK( qvkCmdBindVertexBuffers(vk.command_buffer, 1, multitexture ? 3 : 2, bufs, offs) );
    shadingDat.color_st_elements += tess.numVertexes;

    // bind descriptor sets

    //    vkCmdBindDescriptorSets causes the sets numbered [firstSet.. firstSet+descriptorSetCount-1] to use
    //    the bindings stored in pDescriptorSets[0..descriptorSetCount-1] for subsequent rendering commands 
    //    (either compute or graphics, according to the pipelineBindPoint).
    //    Any bindings that were previously applied via these sets are no longer valid.

    NO_CHECK( qvkCmdBindDescriptorSets(vk.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, 
                vk.pipeline_layout, 0, (multitexture ? 2 : 1), shadingDat.curDescriptorSets, 0, NULL) );

    // bind pipeline
    NO_CHECK( qvkCmdBindPipeline(vk.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline) );

    // configure pipeline's dynamic state
    VkViewport viewport;

    //    VkRect2D scissor = vk.renderArea; 
    //, VkRect2D* const pRect , &scissor
    //vk_setViewportScissor(backEnd.projection2D, depRg, &viewport);

    if (is2D)
    {
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = vk.renderArea.extent.width;
        viewport.height = vk.renderArea.extent.height;
    }
    else
    {
        int X = backEnd.viewParms.viewportX;
        int Y = backEnd.viewParms.viewportY;
        int W = backEnd.viewParms.viewportWidth;
        int H = backEnd.viewParms.viewportHeight;

        //pRect->offset.x = backEnd.viewParms.viewportX;
        //pRect->offset.y = backEnd.viewParms.viewportY;
        //pRect->extent.width = backEnd.viewParms.viewportWidth;
        //pRect->extent.height = backEnd.viewParms.viewportHeight;

        if ( X < 0)
            X = 0;
        if (Y < 0)
            Y = 0;
        if (X + W > vk.renderArea.extent.width)
            W = vk.renderArea.extent.width - X;
        if (Y + H > vk.renderArea.extent.height)
            H = vk.renderArea.extent.height - Y;

        //pRect->offset.x = 
        viewport.x = X;
        //pRect->offset.y = 
        viewport.y = Y;
        //pRect->extent.width =
        viewport.width = W;
        //pRect->extent.height = 
        viewport.height = H;
    }

    switch(depRg)
    {
        case DEPTH_RANGE_NORMAL:
            {
                viewport.minDepth = 0.0f;
                viewport.maxDepth = 1.0f;
            }break;

        case DEPTH_RANGE_ZERO:
            {
                viewport.minDepth = 0.0f;
                viewport.maxDepth = 0.0f;
            }break;

        case DEPTH_RANGE_ONE:
            {
                viewport.minDepth = 1.0f;
                viewport.maxDepth = 1.0f;
            }break;

        case DEPTH_RANGE_WEAPON:
            {
                viewport.minDepth = 0.0f;
                viewport.maxDepth = 0.3f;
            }break;
    }

    NO_CHECK( qvkCmdSetViewport(vk.command_buffer, 0, 1, &viewport) );

    //    qvkCmdSetScissor(vk.command_buffer, 0, 1, &scissor);

    if (tess.shader->polygonOffset) {
        NO_CHECK( qvkCmdSetDepthBias(vk.command_buffer, r_offsetUnits->value, 0.0f, r_offsetFactor->value) );
    }

    // issue draw call
    if (indexed)
    {
        NO_CHECK( qvkCmdDrawIndexed(vk.command_buffer, tess.numIndexes, 1, 0, 0, 0) );
    }
    else
    {
        NO_CHECK( qvkCmdDraw(vk.command_buffer, tess.numVertexes, 1, 0, 0) );
    }

    shadingDat.s_depth_attachment_dirty = VK_TRUE;
}



void updateMVP(VkBool32 isPortal, VkBool32 is2D, const float mvMat4x4[16])
{
    // push constants are another way of passing dynamic values to shaders
    if (is2D)
    {            
        NO_CHECK( qvkCmdPushConstants(vk.command_buffer, vk.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, s_ProjectMat2d) );
    }
    else
    {
        // 3D, mvp transform + eye transform + clipping plane in eye space
        float push_constants[32] QALIGN(16); 
        // update q3's proj matrix (opengl) to vulkan conventions:
        // z - [0, 1] instead of [-1, 1] and invert y direction
        // Eye space transform.
        uint32_t push_size = 64;
        MatrixMultiply4x4_SSE(mvMat4x4, backEnd.viewParms.projectionMatrix, push_constants);
        // As described above in section Pipeline Layouts, the pipeline layout defines shader push constants
        // which are updated via Vulkan commands rather than via writes to memory or copy commands.
        // Push constants represent a high speed path to modify constant data in pipelines
        // that is expected to outperform memory-backed resource updates.

        if (isPortal)
        {
            push_size += 64;
            // NOTE: backEnd.or.modelMatrix incorporates s_flipMatrix,
            // so it should be taken into account when computing clipping plane too.
            
            R_SetPushConstForPortal(&push_constants[16]);
        }

        // As described above in section Pipeline Layouts, the pipeline layout defines shader push constants
        // which are updated via Vulkan commands rather than via writes to memory or copy commands.
        // Push constants represent a high speed path to modify constant data in pipelines
        // that is expected to outperform memory-backed resource updates.
        NO_CHECK( qvkCmdPushConstants(vk.command_buffer, vk.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, push_size, push_constants) );
    }
}


// =========================================================
// Vertex fetching is controlled via configurable state, 
// as a logically distinct graphics pipeline stage.
//  
//  Vertex Attributes
//
//  Vertex shaders can define input variables, which receive vertex attribute data
//  transferred from one or more VkBuffer(s) by drawing commands. Vertex shader 
//  input variables are bound to buffers via an indirect binding where the vertex 
//  shader associates a vertex input attribute number with each variable, vertex 
//  input attributes are associated to vertex input bindings on a per-pipeline basis, 
//  and vertex input bindings are associated with specific buffers on a per-draw basis
//  via the vkCmdBindVertexBuffers command. 
//
//  Vertex input attribute and vertex input binding descriptions also
//  contain format information controlling how data is extracted from
//  buffer memory and converted to the format expected by the vertex shader.
//
//  There are VkPhysicalDeviceLimits::maxVertexInputAttributes number of vertex
//  input attributes and VkPhysicalDeviceLimits::maxVertexInputBindings number of
//  vertex input bindings (each referred to by zero-based indices), where there 
//  are at least as many vertex input attributes as there are vertex input bindings.
//  Applications can store multiple vertex input attributes interleaved in a single 
//  buffer, and use a single vertex input binding to access those attributes.
//
//  In GLSL, vertex shaders associate input variables with a vertex input attribute
//  number using the location layout qualifier. The component layout qualifier
//  associates components of a vertex shader input variable with components of
//  a vertex input attribute.

void vk_UploadXYZI(float (*pXYZ)[4], uint32_t nVertex, uint32_t* pIdx, uint32_t nIndex)
{
	// xyz stream
	{
        const VkDeviceSize xyz_offset = XYZ_OFFSET + shadingDat.xyz_elements * sizeof(vec4_t);
		
        unsigned char* const vDst = shadingDat.vertex_buffer_ptr + xyz_offset;

        // 4 float in the array, with each 4 bytes.
		memcpy(vDst, pXYZ, nVertex * 16);

		NO_CHECK( qvkCmdBindVertexBuffers(vk.command_buffer, 0, 1, &shadingDat.vertex_buffer, &xyz_offset) );
		
        shadingDat.xyz_elements += tess.numVertexes;

        assert (shadingDat.xyz_elements * sizeof(vec4_t) < XYZ_SIZE);
	}

	// indexes stream
    if(nIndex != 0)
	{
		const uint32_t indexes_size = nIndex * sizeof(uint32_t);        

		unsigned char* iDst = shadingDat.index_buffer_ptr + shadingDat.index_buffer_offset;
		memcpy(iDst, pIdx, indexes_size);

		NO_CHECK( qvkCmdBindIndexBuffer(vk.command_buffer, shadingDat.index_buffer,
                    shadingDat.index_buffer_offset, VK_INDEX_TYPE_UINT32) );
		
        shadingDat.index_buffer_offset += indexes_size;

        assert (shadingDat.index_buffer_offset < INDEX_BUFFER_SIZE);
	}
}


void vk_resetGeometryBuffer(void)
{
	// Reset geometry buffer's current offsets.
	shadingDat.xyz_elements = 0;
	shadingDat.color_st_elements = 0;
	shadingDat.index_buffer_offset = 0;
//    shadingDat.s_depth_attachment_dirty = VK_FALSE;

    Mat4Identity(s_modelview_matrix);
}


void vk_destroy_shading_data(void)
{
    ri.Printf(PRINT_ALL, " Destroy vertex/index buffer: shadingDat.vertex_buffer shadingDat.index_buffer. \n");
    ri.Printf(PRINT_ALL, " Free device memory: vertex_buffer_memory index_buffer_memory. \n");

    NO_CHECK( qvkUnmapMemory(vk.device, shadingDat.vertex_buffer_memory) );
    NO_CHECK( qvkUnmapMemory(vk.device, shadingDat.index_buffer_memory) );
	
    NO_CHECK( qvkFreeMemory(vk.device, shadingDat.vertex_buffer_memory, NULL) );
	NO_CHECK( qvkFreeMemory(vk.device, shadingDat.index_buffer_memory, NULL) );

    NO_CHECK( qvkDestroyBuffer(vk.device, shadingDat.vertex_buffer, NULL) );
	NO_CHECK( qvkDestroyBuffer(vk.device, shadingDat.index_buffer, NULL) );

    memset(&shadingDat, 0, sizeof(shadingDat));
}



void vk_clearDepthStencilAttachments(void)
{
    if(shadingDat.s_depth_attachment_dirty)
    {
        VkClearAttachment attachments;

        attachments.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        attachments.clearValue.depthStencil.depth = 1.0f;
        attachments.clearValue.depthStencil.stencil = 0.0f;

        if (r_shadows->integer == 2) {
            attachments.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        VkClearRect clear_rect;
        clear_rect.rect = vk.renderArea;
        clear_rect.baseArrayLayer = 0;
        clear_rect.layerCount = 1;

        //ri.Printf(PRINT_ALL, "(%d, %d, %d, %d)\n", 
        //        clear_rect.rect.offset.x, clear_rect.rect.offset.y, 
        //        clear_rect.rect.extent.width, clear_rect.rect.extent.height);


        NO_CHECK( qvkCmdClearAttachments(vk.command_buffer, 1, &attachments, 1, &clear_rect) );
        
        shadingDat.s_depth_attachment_dirty = VK_FALSE;
    }
}

/*
===================
Perform dynamic lighting with another rendering pass
===================
*/
static void ProjectDlightTexture( shaderCommands_t * const pTess, uint32_t num_dlights, struct dlight_s	* const pDlights)
{
	unsigned char clipBits[SHADER_MAX_VERTEXES];


    uint32_t l;
	for (l = 0; l < num_dlights; ++l)
    {
		//dlight_t	*dl;

		if ( !( pTess->dlightBits & ( 1 << l ) ) )
        {
			continue;	// this surface definately doesn't have any of this light
		}
		float* texCoords = pTess->svars.texcoords[0][0];
		//colors = tess.svars.colors[0];

		//dl = &backEnd.refdef.dlights[l];
        vec3_t origin;
		VectorCopy( pDlights[l].transformed, origin );


        float radius = pDlights[l].radius;
		float scale = 1.0f / radius;
    	float modulate;

        float floatColor[3] = {
		    pDlights[l].color[0] * 255.0f,
		    pDlights[l].color[1] * 255.0f,
		    pDlights[l].color[2] * 255.0f
        };

        uint32_t i;
		for ( i = 0 ; i < pTess->numVertexes ; ++i, texCoords += 2)
        {
			vec3_t dist;

			++backEnd.pc.c_dlightVertexes;

			VectorSubtract( origin, pTess->xyz[i], dist );
			texCoords[0] = 0.5f + dist[0] * scale;
			texCoords[1] = 0.5f + dist[1] * scale;

			uint32_t clip = 0;
			if ( texCoords[0] < 0.0f ) {
				clip |= 1;
			}
            else if ( texCoords[0] > 1.0f ) {
				clip |= 2;
			}

			if ( texCoords[1] < 0.0f ) {
				clip |= 4;
			}
            else if ( texCoords[1] > 1.0f ) {
				clip |= 8;
			}

			// modulate the strength based on the height and color
			if ( dist[2] > radius )
            {
				clip |= 16;
				modulate = 0.0f;
			}
            else if ( dist[2] < -radius )
            {
				clip |= 32;
				modulate = 0.0f;
			}
            else
            {
				dist[2] = fabs(dist[2]);
				if ( dist[2] < radius * 0.5f )
                {
					modulate = 1.0f;
				}
                else
                {
					modulate = 2.0f * (radius - dist[2]) * scale;
				}
			}
			clipBits[i] = clip;
            
            // += 4 
			pTess->svars.colors[i][0] = (floatColor[0] * modulate);
			pTess->svars.colors[i][1] = (floatColor[1] * modulate);
			pTess->svars.colors[i][2] = (floatColor[2] * modulate);
			pTess->svars.colors[i][3] = 255;
		}

      
		// build a list of triangles that need light
		uint32_t numIndexes = 0;
		for ( i = 0 ; i < pTess->numIndexes ; i += 3 )
        {
			uint32_t a, b, c;

			a = pTess->indexes[i];
			b = pTess->indexes[i+1];
			c = pTess->indexes[i+2];
			if ( clipBits[a] & clipBits[b] & clipBits[c] ) {
				continue;	// not lighted
			}
			numIndexes += 3;
		}

		if ( numIndexes == 0 ) {
			continue;
		}


		updateCurDescriptor( tr.dlightImage->descriptor_set, 0 );
		// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light where they aren't rendered
		backEnd.pc.c_totalIndexes += numIndexes;
		backEnd.pc.c_dlightIndexes += numIndexes;

		// VULKAN

		vk_shade_geometry(g_globalPipelines.dlight_pipelines[pDlights[l].additive > 0 ? 1 : 0][pTess->shader->cullType][pTess->shader->polygonOffset],
               backEnd.projection2D, VK_FALSE, DEPTH_RANGE_NORMAL, VK_TRUE);

	}
}




static void RB_FogPass( shaderCommands_t * const pTess )
{

    RB_SetTessFogColor(pTess->svars.colors, pTess->fogNum, pTess->numVertexes);

	RB_CalcFogTexCoords( ( float * ) pTess->svars.texcoords[0] );

	updateCurDescriptor( tr.fogImage->descriptor_set, 0);

	// VULKAN

    assert(pTess->shader->fogPass > 0);
    VkPipeline pipeline = g_globalPipelines.fog_pipelines[pTess->shader->fogPass - 1][pTess->shader->cullType][pTess->shader->polygonOffset];
    vk_shade_geometry(pipeline, VK_FALSE, VK_FALSE, DEPTH_RANGE_NORMAL, VK_TRUE);
}

///////////////////////////////////////////////////////////////////////////////////

static void R_BindAnimatedImage( textureBundle_t *bundle, int tmu, float time)
{

	if ( bundle->isVideoMap ) {
		ri.CIN_RunCinematic(bundle->videoMapHandle);
		ri.CIN_UploadCinematic(bundle->videoMapHandle);
		return;
	}

	if ( bundle->numImageAnimations <= 1 ) {
		updateCurDescriptor( bundle->image[0]->descriptor_set, tmu );
		return;
	}

	// it is necessary to do this messy calc to make sure animations line up
	// exactly with waveforms of the same frequency
	int index = (int)( time * bundle->imageAnimationSpeed * FUNCTABLE_SIZE );
	index >>= FUNCTABLE_SIZE2;

	if ( index < 0 ) {
		index = 0;	// may happen with shader time offsets
	}
	index %= bundle->numImageAnimations;

	updateCurDescriptor( bundle->image[ index ]->descriptor_set, tmu );
}

/////////////////////////////////////////////////////////////////////////////////

void RB_StageIteratorGeneric(shaderCommands_t * const pTess, VkBool32 is2D)
{
//	shaderCommands_t *input = &tess;

	RB_DeformTessGeometry(pTess);

	// call shader function
	//
	// VULKAN
   
    vk_UploadXYZI(pTess->xyz, pTess->numVertexes, pTess->indexes, pTess->numIndexes);

    updateMVP(backEnd.viewParms.isPortal, is2D, getptr_modelview_matrix() );
    

    uint32_t stage = 0;

	for ( stage = 0; stage < MAX_SHADER_STAGES; ++stage )
	{
		if ( NULL == pTess->xstages[stage])
		{
			break;
		}

		RB_ComputeColors( pTess->xstages[stage] );
		RB_ComputeTexCoords( pTess->xstages[stage] );

        // base, set state
		R_BindAnimatedImage( &pTess->xstages[stage]->bundle[0], 0, pTess->shaderTime );
		//
		// do multitexture
		//
        qboolean multitexture = (pTess->xstages[stage]->bundle[1].image[0] != NULL);

		if ( multitexture )
		{
            // DrawMultitextured( input, stage );
            // output = t0 * t1 or t0 + t1

            // t0 = most upstream according to spec
            // t1 = most downstream according to spec
            // this is an ugly hack to work around a GeForce driver
            // bug with multitexture and clip planes


            // lightmap/secondary pass

            R_BindAnimatedImage( &pTess->xstages[stage]->bundle[1], 1, pTess->shaderTime );
            // disable texturing on TEXTURE1, then select TEXTURE0
		}

       
        enum Vk_Depth_Range depth_range = DEPTH_RANGE_NORMAL;
        if (pTess->shader->isSky)
        {
            depth_range = DEPTH_RANGE_ONE;
            if (r_showsky->integer)
                depth_range = DEPTH_RANGE_ZERO;
        }
        else if (backEnd.currentEntity->e.renderfx & RF_DEPTHHACK)
        {
            depth_range = DEPTH_RANGE_WEAPON;
        }


        if ( r_lightmap->integer && multitexture) {
            updateCurDescriptor( tr.whiteImage->descriptor_set, 0 );
        }

        if (backEnd.viewParms.isMirror)
        {
            vk_shade_geometry(pTess->xstages[stage]->vk_mirror_pipeline, multitexture, 0, depth_range, VK_TRUE);
        }
        else if (backEnd.viewParms.isPortal)
        {
            vk_shade_geometry(pTess->xstages[stage]->vk_portal_pipeline, multitexture, 0, depth_range, VK_TRUE);
        }
        else
        {
            vk_shade_geometry(pTess->xstages[stage]->vk_pipeline, multitexture, is2D, depth_range, VK_TRUE);
        }

                
		// allow skipping out to show just lightmaps during development
		if ( r_lightmap->integer && ( pTess->xstages[stage]->bundle[0].isLightmap || pTess->xstages[stage]->bundle[1].isLightmap ) )
		{
			break;
		}
	}

	// 
	// now do any dynamic lighting needed
	//
	if ( pTess->dlightBits && 
         pTess->shader->sort <= SS_OPAQUE && 
            !(pTess->shader->surfaceFlags & (SURF_NODLIGHT | SURF_SKY) ) )
    {
        if(backEnd.refdef.num_dlights != 0)
        {
	    	ProjectDlightTexture( pTess, backEnd.refdef.num_dlights, backEnd.refdef.dlights);
        }
	}

	//
	// now do fog
	//
	if ( pTess->fogNum && pTess->shader->fogPass ) {
		RB_FogPass(pTess);
	}
}


