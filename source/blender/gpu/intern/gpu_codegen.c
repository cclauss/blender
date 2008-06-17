/**
 * $Id$
 *
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version. The Blender
 * Foundation also sells licenses for use in proprietary software under
 * the Blender License.  See http://www.blender.org/BL/ for information
 * about this.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * The Original Code is Copyright (C) 2005 Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Brecht Van Lommel.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "GL/glew.h"

#include "MEM_guardedalloc.h"

#include "DNA_customdata_types.h"
#include "DNA_image_types.h"
#include "DNA_listBase.h"

#include "BLI_dynstr.h"
#include "BLI_blenlib.h"
#include "BLI_ghash.h"
#include "BLI_heap.h"

#include "BKE_global.h"
#include "BKE_utildefines.h"

#include "GPU_material.h"
#include "GPU_extensions.h"

#include "gpu_codegen.h"
#include "material_vertex_shader.glsl.c"
#include "material_shaders.glsl.c"

#include <string.h>
#include <stdarg.h>

#ifdef _WIN32
#ifndef vsnprintf
#define _vsnprintf vsnprintf
#endif
#ifndef snprintf
#define snprintf _snprintf
#endif
#endif

/* structs and defines */

#define GPU_VEC_UNIFORM	1
#define GPU_TEX_PIXEL 	2
#define GPU_TEX_RAND 	3
#define GPU_ARR_UNIFORM	4
#define GPU_S_ATTRIB	5

static char* GPU_DATATYPE_STR[17] = {"", "float", "vec2", "vec3", "vec4",
	0, 0, 0, 0, "mat3", 0, 0, 0, 0, 0, 0, "mat4"};

struct GPUNode {
	struct GPUNode *next, *prev;

	char *name;

	ListBase inputs;
	ListBase outputs;
};

struct GPUNodeLink {
	GPUNodeStack *socket;

	int attribtype;
	char *attribname;

	int image;

	int texture;
	int texturesize;

	void *ptr1, *ptr2;

	int dynamic;

	int type;
	int users;

	struct GPUOutput *source;
};

typedef struct GPUOutput {
	struct GPUOutput *next, *prev;

	GPUNode *node;
	int type;				/* data type = length of vector/matrix */
	GPUNodeLink *link;		/* output link */
	int id;					/* unique id as created by code generator */
} GPUOutput;

typedef struct GPUInput {
	struct GPUInput *next, *prev;

	GPUNode *node;

	int type;				/* datatype */
	int arraysize;			/* number of elements in an array */
	int samp;

	int id;					/* unique id as created by code generator */
	int texid;				/* number for multitexture */
	int attribid;			/* id for vertex attributes */
	int bindtex;			/* input is responsible for binding the texture? */
	int definetex;			/* input is responsible for defining the pixel? */
	int textarget;			/* GL_TEXTURE_* */

	float vec[16];			/* vector data */
	float *dynamicvec;		/* vector data in case it is dynamic */
	GPUNodeLink *link;
	GPUTexture *tex;		/* input texture, only set at runtime */
	struct Image *ima;		/* image */
	struct ImageUser *iuser;/* image user */
	int attribtype;			/* attribute type */
	char attribname[32];	/* attribute name */
	int attribfirst;		/* this is the first one that is bound */
} GPUInput;

struct GPUPass {
	struct GPUPass *next, *prev;

	ListBase nodes;
	int firstbind;
	struct GPUOutput *output;
	struct GPUShader *shader;
};

/* GLSL code parsing for finding function definitions.
 * These are stored in a hash for lookup when creating a material. */

static GHash *FUNCTION_HASH= NULL;

static int gpu_str_prefix(char *str, char *prefix)
{
	while(*str && *prefix) {
		if(*str != *prefix)
			return 0;

		str++;
		prefix++;
	}
	
	return (*prefix == '\0');
}

static char *gpu_str_skip_token(char *str, char *token, int max)
{
	int len = 0;

	/* skip a variable/function name */
	while(*str) {
		if(ELEM6(*str, ' ', '(', ')', ',', '\t', '\n'))
			break;
		else {
			if(token && len < max-1) {
				*token= *str;
				token++;
				len++;
			}
			str++;
		}
	}

	if(token)
		*token= '\0';

	/* skip the next special characters:
	 * note the missing ')' */
	while(*str) {
		if(ELEM5(*str, ' ', '(', ',', '\t', '\n'))
			str++;
		else
			break;
	}

	return str;
}

static void gpu_parse_functions_string(GHash *hash, char *code)
{
	GPUFunction *function;
	int i, type, out;

	while((code = strstr(code, "void "))) {
		function = MEM_callocN(sizeof(GPUFunction), "GPUFunction");

		code = gpu_str_skip_token(code, NULL, 0);
		code = gpu_str_skip_token(code, function->name, MAX_FUNCTION_NAME);

		/* get parameters */
		while(*code && *code != ')') {
			/* test if it's an input or output */
			out= gpu_str_prefix(code, "out ");
			if(out || gpu_str_prefix(code, "in ") || gpu_str_prefix(code, "inout "))
				code = gpu_str_skip_token(code, NULL, 0);

			/* test for type */
			type= 0;
			for(i=1; i<=16; i++) {
				if(GPU_DATATYPE_STR[i] && gpu_str_prefix(code, GPU_DATATYPE_STR[i])) {
					type= i;
					break;
				}
			}

			if(!type && gpu_str_prefix(code, "sampler1D"))
				type= GPU_TEX1D;
			if(!type && gpu_str_prefix(code, "sampler2D"))
				type= GPU_TEX2D;

			if(type) {
				/* add paramater */
				code = gpu_str_skip_token(code, NULL, 0);
				code = gpu_str_skip_token(code, NULL, 0);
				function->paramout[function->totparam]= out;
				function->paramtype[function->totparam]= type;
				function->totparam++;
			}
			else {
				fprintf(stderr, "GPU invalid function parameter in %s.\n", function->name);
				break;
			}
		}

		if(strlen(function->name) == 0 || function->totparam == 0) {
			fprintf(stderr, "GPU functions parse error.\n");
			MEM_freeN(function);
			break;
		}

		BLI_ghash_insert(hash, function->name, function);
	}
}

GPUFunction *GPU_lookup_function(char *name)
{
	if(!FUNCTION_HASH) {
		FUNCTION_HASH= BLI_ghash_new(BLI_ghashutil_strhash, BLI_ghashutil_strcmp);
		gpu_parse_functions_string(FUNCTION_HASH, datatoc_material_shaders_glsl);
	}

	return (GPUFunction*)BLI_ghash_lookup(FUNCTION_HASH, name);
}

void GPU_extensions_exit(void)
{
	if(FUNCTION_HASH)
		BLI_ghash_free(FUNCTION_HASH, NULL, (GHashValFreeFP)MEM_freeN);
}

/* Strings utility */

static void BLI_dynstr_printf(DynStr *dynstr, const char *format, ...)
{
	va_list args;
	int retval;
	char str[2048];

	/* todo: windows support */
	va_start(args, format);
	retval = vsnprintf(str, sizeof(str), format, args);
	va_end(args);

	if (retval >= sizeof(str))
		fprintf(stderr, "BLI_dynstr_printf: limit exceeded\n");
	else
		BLI_dynstr_append(dynstr, str);
}

/* GLSL code generation */

static void codegen_convert_datatype(DynStr *ds, int from, int to, char *tmp, int id)
{
	char name[1024];

	snprintf(name, sizeof(name), "%s%d", tmp, id);

	if (from == to) {
		BLI_dynstr_append(ds, name);
	}
	else if (to == GPU_FLOAT) {
		if (from == GPU_VEC4)
			BLI_dynstr_printf(ds, "dot(%s.rgb, vec3(0.35, 0.45, 0.2))", name);
		else if (from == GPU_VEC3)
			BLI_dynstr_printf(ds, "dot(%s, vec3(0.33))", name);
		else if (from == GPU_VEC2)
			BLI_dynstr_printf(ds, "%s.r", name);
	}
	else if (to == GPU_VEC2) {
		if (from == GPU_VEC4)
			BLI_dynstr_printf(ds, "vec2(dot(%s.rgb, vec3(0.35, 0.45, 0.2)), %s.a)", name, name);
		else if (from == GPU_VEC3)
			BLI_dynstr_printf(ds, "vec2(dot(%s.rgb, vec3(0.33)), 1.0)", name);
		else if (from == GPU_FLOAT)
			BLI_dynstr_printf(ds, "vec2(%s, 1.0)", name);
	}
	else if (to == GPU_VEC3) {
		if (from == GPU_VEC4)
			BLI_dynstr_printf(ds, "%s.rgb", name);
		else if (from == GPU_VEC2)
			BLI_dynstr_printf(ds, "vec3(%s.r, %s.r, %s.r)", name, name, name);
		else if (from == GPU_FLOAT)
			BLI_dynstr_printf(ds, "vec3(%s, %s, %s)", name, name, name);
	}
	else {
		if (from == GPU_VEC3)
			BLI_dynstr_printf(ds, "vec4(%s, 1.0)", name);
		else if (from == GPU_VEC2)
			BLI_dynstr_printf(ds, "vec4(%s.r, %s.r, %s.r, %s.g)", name, name, name, name);
		else if (from == GPU_FLOAT)
			BLI_dynstr_printf(ds, "vec4(%s, %s, %s, 1.0)", name, name, name);
	}
}

static void codegen_convert_datatype_texco(DynStr *ds, int to, char *tmp, int id)
{
	char name[1024];

	snprintf(name, sizeof(name), "%s%d", tmp, id);

	if (to == GPU_FLOAT) {
		BLI_dynstr_printf(ds, "%s.x", name);
	}
	else if (to == GPU_VEC2) {
		BLI_dynstr_printf(ds, "%s.xy", name);
	}
	else if (to == GPU_VEC3) {
		BLI_dynstr_printf(ds, "%s.xyz", name);
	}
	else if (to == GPU_VEC4) {
		BLI_dynstr_append(ds, name);
	}
}

static int codegen_input_has_texture(GPUInput *input)
{
	if (input->link)
		return 0;
	else if(input->ima)
		return 1;
	else
		return input->tex != 0;
}

static void codegen_set_unique_ids(ListBase *nodes)
{
	GHash *bindhash, *definehash;
	GPUNode *node;
	GPUInput *input;
	GPUOutput *output;
	int id = 1, texid = 0;

	bindhash= BLI_ghash_new(BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp);
	definehash= BLI_ghash_new(BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp);

	for (node=nodes->first; node; node=node->next) {
		for (input=node->inputs.first; input; input=input->next) {
			/* set id for unique names of uniform variables */
			input->id = id++;
			input->bindtex = 0;
			input->definetex = 0;

			/* set texid used for settings texture slot with multitexture */
			if (codegen_input_has_texture(input) &&
			    ((input->samp == GPU_TEX_RAND) || (input->samp == GPU_TEX_PIXEL))) {
				if (input->link) {
					/* input is texture from buffer, assign only one texid per
					   buffer to avoid sampling the same texture twice */
					if (!BLI_ghash_haskey(bindhash, input->link)) {
						input->texid = texid++;
						input->bindtex = 1;
						BLI_ghash_insert(bindhash, input->link, SET_INT_IN_POINTER(input->texid));
					}
					else
						input->texid = GET_INT_FROM_POINTER(BLI_ghash_lookup(bindhash, input->link));
				}
				else if(input->ima) {
					/* input is texture from image, assign only one texid per
					   buffer to avoid sampling the same texture twice */
					if (!BLI_ghash_haskey(bindhash, input->ima)) {
						input->texid = texid++;
						input->bindtex = 1;
						BLI_ghash_insert(bindhash, input->ima, SET_INT_IN_POINTER(input->texid));
					}
					else
						input->texid = GET_INT_FROM_POINTER(BLI_ghash_lookup(bindhash, input->ima));
				}
				else {
					/* input is user created texture, we know there there is
					   only one, so assign new texid */
					input->bindtex = 1;
					input->texid = texid++;
				}

				/* make sure this pixel is defined exactly once */
				if (input->samp == GPU_TEX_PIXEL) {
					if(input->ima) {
						if (!BLI_ghash_haskey(definehash, input->ima)) {
							input->definetex = 1;
							BLI_ghash_insert(definehash, input->ima, SET_INT_IN_POINTER(input->texid));
						}
					}
					else {
						if (!BLI_ghash_haskey(definehash, input->link)) {
							input->definetex = 1;
							BLI_ghash_insert(definehash, input->link, SET_INT_IN_POINTER(input->texid));
						}
					}
				}
			}
		}

		for (output=node->outputs.first; output; output=output->next)
			/* set id for unique names of tmp variables storing output */
			output->id = id++;
	}

	BLI_ghash_free(bindhash, NULL, NULL);
	BLI_ghash_free(definehash, NULL, NULL);
}

static void codegen_print_uniforms_functions(DynStr *ds, ListBase *nodes)
{
	GPUNode *node;
	GPUInput *input;

	/* print uniforms */
	for (node=nodes->first; node; node=node->next) {
		for (input=node->inputs.first; input; input=input->next) {
			if ((input->samp == GPU_TEX_RAND) || (input->samp == GPU_TEX_PIXEL)) {
				/* create exactly one sampler for each texture */
				if (codegen_input_has_texture(input) && input->bindtex)
					BLI_dynstr_printf(ds, "uniform %s samp%d;\n",
						(input->textarget == GL_TEXTURE_1D)? "sampler1D": "sampler2D",
						input->texid);
			}
			else if (input->samp == GPU_VEC_UNIFORM) {
				/* and create uniform vectors or matrices for all vectors */
				BLI_dynstr_printf(ds, "uniform %s unf%d;\n",
					GPU_DATATYPE_STR[input->type], input->id);
			}
			else if (input->samp == GPU_ARR_UNIFORM) {
				BLI_dynstr_printf(ds, "uniform %s unf%d[%d];\n",
					GPU_DATATYPE_STR[input->type], input->id, input->arraysize);
			}
			else if (input->samp == GPU_S_ATTRIB && input->attribfirst) {
				BLI_dynstr_printf(ds, "varying %s var%d;\n",
					GPU_DATATYPE_STR[input->type], input->attribid);
			}
		}
	}

	BLI_dynstr_append(ds, "\n");

	BLI_dynstr_append(ds, datatoc_material_shaders_glsl);
}

static void codegen_declare_tmps(DynStr *ds, ListBase *nodes)
{
	GPUNode *node;
	GPUInput *input;
	GPUOutput *output;

	for (node=nodes->first; node; node=node->next) {
		/* load pixels from textures */
		for (input=node->inputs.first; input; input=input->next) {
			if (input->samp == GPU_TEX_PIXEL) {
				if (codegen_input_has_texture(input) && input->definetex) {
					BLI_dynstr_printf(ds, "\tvec4 tex%d = texture2D(", input->texid);
					BLI_dynstr_printf(ds, "samp%d, gl_TexCoord[%d].st);\n",
						input->texid, input->texid);
				}
			}
		}

		/* declare temporary variables for node output storage */
		for (output=node->outputs.first; output; output=output->next)
			BLI_dynstr_printf(ds, "\t%s tmp%d;\n",
				GPU_DATATYPE_STR[output->type], output->id);
	}

	BLI_dynstr_append(ds, "\n");
}

static void codegen_call_functions(DynStr *ds, ListBase *nodes, GPUOutput *finaloutput)
{
	GPUNode *node;
	GPUInput *input;
	GPUOutput *output;

	for (node=nodes->first; node; node=node->next) {
		BLI_dynstr_printf(ds, "\t%s(", node->name);
		
		for (input=node->inputs.first; input; input=input->next) {
			if (input->samp == GPU_TEX_RAND) {
				BLI_dynstr_printf(ds, "samp%d", input->texid);
				if (input->link)
					BLI_dynstr_printf(ds, ", gl_TexCoord[%d].st", input->texid);
			}
			else if (input->samp == GPU_TEX_PIXEL) {
				if (input->link && input->link->source)
					codegen_convert_datatype(ds, input->link->source->type, input->type,
						"tmp", input->link->source->id);
				else
					codegen_convert_datatype(ds, input->link->source->type, input->type,
						"tex", input->texid);
			}
			else if ((input->samp == GPU_VEC_UNIFORM) ||
			         (input->samp == GPU_ARR_UNIFORM))
				BLI_dynstr_printf(ds, "unf%d", input->id);
			else if (input->samp == GPU_S_ATTRIB)
				BLI_dynstr_printf(ds, "var%d", input->attribid);

			BLI_dynstr_append(ds, ", ");
		}

		for (output=node->outputs.first; output; output=output->next) {
			BLI_dynstr_printf(ds, "tmp%d", output->id);
			if (output->next)
				BLI_dynstr_append(ds, ", ");
		}

		BLI_dynstr_append(ds, ");\n");
	}

	BLI_dynstr_append(ds, "\n\tgl_FragColor = ");
	codegen_convert_datatype(ds, finaloutput->type, GPU_VEC4, "tmp", finaloutput->id);
	BLI_dynstr_append(ds, ";\n");
}

static char *code_generate_fragment(ListBase *nodes, GPUOutput *output)
{
	DynStr *ds = BLI_dynstr_new();
	char *code;

	codegen_set_unique_ids(nodes);
	codegen_print_uniforms_functions(ds, nodes);

	BLI_dynstr_append(ds, "void main(void)\n");
	BLI_dynstr_append(ds, "{\n");

	codegen_declare_tmps(ds, nodes);
	codegen_call_functions(ds, nodes, output);

	BLI_dynstr_append(ds, "}\n");

	/* create shader */
	code = BLI_dynstr_get_cstring(ds);
	BLI_dynstr_free(ds);

	//if(G.f & G_DEBUG) printf("%s\n", code);

	return code;
}

static char *code_generate_vertex(ListBase *nodes, int profile)
{
	DynStr *ds = BLI_dynstr_new();
	GPUNode *node;
	GPUInput *input;
	char *code;
	
	for (node=nodes->first; node; node=node->next) {
		for (input=node->inputs.first; input; input=input->next) {
			if (input->samp == GPU_S_ATTRIB && input->attribfirst) {
				if(profile == GPU_PROFILE_DERIVEDMESH)
					BLI_dynstr_printf(ds, "attribute %s att%d;\n",
						GPU_DATATYPE_STR[input->type], input->attribid);
				BLI_dynstr_printf(ds, "varying %s var%d;\n",
					GPU_DATATYPE_STR[input->type], input->attribid);
			}
		}
	}

	BLI_dynstr_append(ds, "\n");
	BLI_dynstr_append(ds, datatoc_material_vertex_shader_glsl);

	for (node=nodes->first; node; node=node->next)
		for (input=node->inputs.first; input; input=input->next)
			if (input->samp == GPU_S_ATTRIB && input->attribfirst) {
				if(input->attribtype == CD_TANGENT) /* silly exception */
					BLI_dynstr_printf(ds, "\tvar%d = gl_NormalMatrix * ", input->attribid);
				else
					BLI_dynstr_printf(ds, "\tvar%d = ", input->attribid);

				if(profile == GPU_PROFILE_DERIVEDMESH) {
					BLI_dynstr_printf(ds, "att%d;\n", input->attribid);
				}
				else if(profile == GPU_PROFILE_GAME) {
					codegen_convert_datatype_texco(ds, input->type, "gl_MultiTexCoord", input->attribid);
					BLI_dynstr_append(ds, ";\n");
				}
			}

	BLI_dynstr_append(ds, "}\n\n");

	code = BLI_dynstr_get_cstring(ds);

	BLI_dynstr_free(ds);

	//if(G.f & G_DEBUG) printf("%s\n", code);

	return code;
}

/* GPU pass binding/unbinding */

GPUShader *GPU_pass_shader(GPUPass *pass)
{
	return pass->shader;
}

void GPU_pass_bind(GPUPass *pass)
{
	GPUNode *node;
	GPUInput *input;
	GPUShader *shader = pass->shader;
	ListBase *nodes = &pass->nodes;
	DynStr *ds;
	char *name;

	if (!shader)
		return;

	/* create textures first, otherwise messes up multitexture state for
	 * following textures*/
	for (node=nodes->first; node; node=node->next)
		for (input=node->inputs.first; input; input=input->next)
			if (input->ima)
				input->tex = GPU_texture_from_blender(input->ima, input->iuser);

	GPU_shader_bind(shader);

	for (node=nodes->first; node; node=node->next) {
		for (input=node->inputs.first; input; input=input->next) {
			/* attributes don't need to be bound, they already have
			 * an id that the drawing functions will use */
			if(input->samp == GPU_S_ATTRIB)
				continue;

			/* pass samplers and uniforms to opengl */
			if (input->link)
				input->tex = NULL; /* input->link->tex; */

			ds = BLI_dynstr_new();
			if (input->tex)
				BLI_dynstr_printf(ds, "samp%d", input->texid);
			else
				BLI_dynstr_printf(ds, "unf%d", input->id);
			name = BLI_dynstr_get_cstring(ds);
			BLI_dynstr_free(ds);

			if (input->tex) {
				if (input->bindtex) {
					if(pass->firstbind);
					GPU_texture_bind(input->tex, input->texid);
					GPU_shader_uniform_texture(shader, name, input->tex);
				}
			}
			else if (input->arraysize) {
				if(pass->firstbind || input->dynamicvec)
					GPU_shader_uniform_vector(shader, name, input->type,
						input->arraysize,
						(input->dynamicvec)? input->dynamicvec: input->vec);
			}
			else {
				if(pass->firstbind || input->dynamicvec)
					GPU_shader_uniform_vector(shader, name, input->type, 1,
						(input->dynamicvec)? input->dynamicvec: input->vec);
			}

			MEM_freeN(name);
		}
	}

	pass->firstbind = 0;
}

void GPU_pass_unbind(GPUPass *pass)
{
	GPUNode *node;
	GPUInput *input;
	GPUShader *shader = pass->shader;
	ListBase *nodes = &pass->nodes;

	if (!shader)
		return;

	for (node=nodes->first; node; node=node->next) {
		for (input=node->inputs.first; input; input=input->next) {
			if (input->tex)
				if(input->bindtex)
					GPU_texture_unbind(input->tex);
			if (input->link || input->ima)
				input->tex = 0;
		}
	}
	
	GPU_shader_unbind(shader);
}

/* Pass create/free */

GPUPass *GPU_generate_pass(ListBase *nodes, struct GPUNodeLink *outlink, int vertexshader, int profile)
{
	GPUShader *shader;
	GPUPass *pass;
	char *vertexcode, *fragmentcode;

	/* generate code and compile with opengl */
	fragmentcode = code_generate_fragment(nodes, outlink->source);
	vertexcode = (vertexshader)? code_generate_vertex(nodes, profile): NULL;
	shader = GPU_shader_create(vertexcode, fragmentcode);
	MEM_freeN(fragmentcode);
	MEM_freeN(vertexcode);

	/* failed? */
	if (!shader) {
		GPU_nodes_free(nodes);
		return NULL;
	}
	
	/* create pass */
	pass = MEM_callocN(sizeof(GPUPass), "GPUPass");

	pass->nodes = *nodes;
	pass->output = outlink->source;
	pass->shader = shader;
	pass->firstbind = 1;

	/* take ownership over nodes */
	memset(nodes, 0, sizeof(*nodes));

	return pass;
}

void GPU_pass_free(GPUPass *pass)
{
	GPU_shader_free(pass->shader);
	GPU_nodes_free(&pass->nodes);
	MEM_freeN(pass);
}

/* Node Link Functions */

GPUNodeLink *GPU_node_link_create(int type)
{
	GPUNodeLink *link = MEM_callocN(sizeof(GPUNodeLink), "GPUNodeLink");
	link->type = type;
	link->users++;

	return link;
}

void GPU_node_link_free(GPUNodeLink *link)
{
	link->users--;

	if (link->users < 0)
		fprintf(stderr, "GPU_node_link_free: negative refcount\n");
	
	if (link->users == 0) {
		if (link->source)
			link->source->link = NULL;
		MEM_freeN(link);
	}
}

/* Node Functions */

GPUNode *GPU_node_begin(char *name)
{
	GPUNode *node = MEM_callocN(sizeof(GPUNode), "GPUNode");

	node->name = name;

	return node;
}

void GPU_node_end(GPUNode *node)
{
	/* empty */
}

void GPU_node_input_array(GPUNode *node, int type, char *name, void *ptr1, void *ptr2, void *ptr3, int dynamic)
{
	GPUInput *input = MEM_callocN(sizeof(GPUInput), "GPUInput");

	input->node = node;
	
	if (type == GPU_ATTRIB) {
		input->type = *((int*)ptr1);
		input->samp = GPU_S_ATTRIB;

		input->attribtype = *((int*)ptr2);
		BLI_strncpy(input->attribname, (char*)ptr3, sizeof(input->attribname));
	}
	else if ((type == GPU_TEX1D) || (type == GPU_TEX2D)) {
		if(ptr1 && ptr2) {
			int length = *((int*)ptr1);
			float *pixels = ((float*)ptr2);

			input->type = GPU_VEC4;
			input->samp = GPU_TEX_RAND;

			if (type == GPU_TEX1D) {
				input->tex = GPU_texture_create_1D(length, pixels, 1);
				input->textarget = GL_TEXTURE_1D;
			}
			else {
				input->tex = GPU_texture_create_2D(length, length, pixels, 1);
				input->textarget = GL_TEXTURE_2D;
			}
		}
		else {
			input->type = GPU_VEC4;
			input->samp = GPU_TEX_RAND;
			input->ima = (Image*)ptr1;
			input->textarget = GL_TEXTURE_2D;
		}
	}
	else {
		float *vec = ((float*)ptr1);
		GPUNodeLink *link = ((GPUNodeLink*)ptr2);
		int length = type;

		input->type = type;

		if (link) {
			input->samp = GPU_TEX_PIXEL;
			input->textarget = GL_TEXTURE_2D;
			input->link = link;
			link->users++;
		}
		else {
			if (ptr3) {
				int arraysize = *((int*)ptr3);
				input->samp = GPU_ARR_UNIFORM;
				input->arraysize = arraysize;
				memcpy(input->vec, vec, length*arraysize*sizeof(float));
				if(dynamic)
					input->dynamicvec= vec;
			}
			else {
				input->samp = GPU_VEC_UNIFORM;
				memcpy(input->vec, vec, length*sizeof(float));
				if(dynamic)
					input->dynamicvec= vec;
			}
		}
	}

	BLI_addtail(&node->inputs, input);
}

void GPU_node_input(GPUNode *node, int type, char *name, void *ptr1, void *ptr2, int dynamic)
{
	GPU_node_input_array(node, type, name, ptr1, ptr2, NULL, dynamic);
}

void GPU_node_output(GPUNode *node, int type, char *name, GPUNodeLink **link)
{
	GPUOutput *output = MEM_callocN(sizeof(GPUOutput), "GPUOutput");

	output->type = type;
	output->node = node;

	if (link) {
		*link = output->link = GPU_node_link_create(type);
		output->link->source = output;

		/* note: the caller owns the reference to the linkfer, GPUOutput
		   merely points to it, and if the node is destroyed it will
		   set that pointer to NULL */
	}

	BLI_addtail(&node->outputs, output);
}

void GPU_node_free(GPUNode *node)
{
	GPUInput *input;
	GPUOutput *output;

	for (input=node->inputs.first; input; input=input->next) {
		if (input->link) {
			GPU_node_link_free(input->link);
		}
		else if (input->tex)
			GPU_texture_free(input->tex);
	}

	for (output=node->outputs.first; output; output=output->next)
		if (output->link) {
			output->link->source = NULL;
			GPU_node_link_free(output->link);
		}

	BLI_freelistN(&node->inputs);
	BLI_freelistN(&node->outputs);
	MEM_freeN(node);
}

void GPU_nodes_free(ListBase *nodes)
{
	GPUNode *node;

	while (nodes->first) {
		node = nodes->first;
		BLI_remlink(nodes, node);
		GPU_node_free(node);
	}
}

/* vertex attributes */

void GPU_nodes_create_vertex_attributes(ListBase *nodes, GPUVertexAttribs *attribs)
{
	GPUNode *node;
	GPUInput *input;
	int a;

	/* convert attributes requested by node inputs to an array of layers,
	 * checking for duplicates and assigning id's starting from zero. */

	memset(attribs, 0, sizeof(*attribs));

	for(node=nodes->first; node; node=node->next) {
		for(input=node->inputs.first; input; input=input->next) {
			if(input->samp == GPU_S_ATTRIB) {
				for(a=0; a<attribs->totlayer; a++) {
					if(attribs->layer[a].type == input->attribtype &&
						strcmp(attribs->layer[a].name, input->attribname) == 0)
						break;
				}

				if(a == attribs->totlayer && a < GPU_MAX_ATTRIB) {
					input->attribid = attribs->totlayer++;
					input->attribfirst = 1;

					attribs->layer[a].type = input->attribtype;
					attribs->layer[a].glindex = input->attribid;
					BLI_strncpy(attribs->layer[a].name, input->attribname,
						sizeof(attribs->layer[a].name));
				}
				else
					input->attribid = attribs->layer[a].glindex;
			}
		}
	}
}

/* varargs linking  */

GPUNodeLink *GPU_attribute(int type, char *name)
{
	GPUNodeLink *link = GPU_node_link_create(0);

	link->attribtype= type;
	link->attribname= name;

	return link;
}

GPUNodeLink *GPU_uniform(float *num)
{
	GPUNodeLink *link = GPU_node_link_create(0);

	link->ptr1= num;
	link->ptr2= NULL;

	return link;
}

GPUNodeLink *GPU_dynamic_uniform(float *num)
{
	GPUNodeLink *link = GPU_node_link_create(0);

	link->ptr1= num;
	link->ptr2= NULL;
	link->dynamic= 1;

	return link;
}

GPUNodeLink *GPU_image(Image *ima, ImageUser *iuser)
{
	GPUNodeLink *link = GPU_node_link_create(0);

	link->image= 1;
	link->ptr1= ima;
	link->ptr2= iuser;

	return link;
}

GPUNodeLink *GPU_texture(int size, float *pixels)
{
	GPUNodeLink *link = GPU_node_link_create(0);

	link->texture = 1;
	link->texturesize = size;
	link->ptr1= pixels;

	return link;
}

GPUNodeLink *GPU_socket(GPUNodeStack *sock)
{
	GPUNodeLink *link = GPU_node_link_create(0);

	link->socket= sock;

	return link;
}

static void gpu_link_input(GPUNode *node, GPUFunction *function, int i, GPUNodeLink *link)
{
	if(link->socket) {
		GPUNodeStack *sock = link->socket;
		GPU_node_input(node, sock->type, sock->name, sock->vec, sock->link, link->dynamic);
		MEM_freeN(link);
	}
	else if(link->texture) {
		GPU_node_input(node, function->paramtype[i], "", &link->texturesize, link->ptr1, link->dynamic);
		MEM_freeN(link->ptr1);
		MEM_freeN(link);
	}
	else if(link->image) {
		GPU_node_input(node, function->paramtype[i], "", link->ptr1, NULL, link->dynamic);
		MEM_freeN(link);
	}
	else if(link->attribtype) {
		GPU_node_input_array(node, GPU_ATTRIB, "", &function->paramtype[i], &link->attribtype, link->attribname, link->dynamic);
		MEM_freeN(link);
	}
	else if(link->ptr1) {
		GPU_node_input(node, function->paramtype[i], "", link->ptr1, NULL, link->dynamic);
		MEM_freeN(link);
	}
	else
		GPU_node_input(node, function->paramtype[i], "", NULL, link, link->dynamic);
}

GPUNode *GPU_link(GPUMaterial *mat, char *name, ...)
{
	GPUNode *node;
	GPUFunction *function;
	GPUNodeLink *link, **linkptr;
	va_list params;
	int i;

	function = GPU_lookup_function(name);
	if(!function) {
		fprintf(stderr, "GPU failed to find function %s\n", name);
		return NULL;
	}

	node = GPU_node_begin(name);

	va_start(params, name);
	for(i=0; i<function->totparam; i++) {
		if(function->paramout[i]) {
			linkptr= va_arg(params, GPUNodeLink**);
			GPU_node_output(node, function->paramtype[i], "", linkptr);
		}
		else {
			link= va_arg(params, GPUNodeLink*);
			gpu_link_input(node, function, i, link);
		}
	}
	va_end(params);

	GPU_node_end(node);

	gpu_material_add_node(mat, node);

	return node;
}

GPUNode *GPU_stack_link(GPUMaterial *mat, char *name, GPUNodeStack *in, GPUNodeStack *out, ...)
{
	GPUNode *node;
	GPUFunction *function;
	GPUNodeLink *link, **linkptr;
	va_list params;
	int i, totin, totout;

	function = GPU_lookup_function(name);
	if(!function) {
		fprintf(stderr, "GPU failed to find function %s\n", name);
		return NULL;
	}

	node = GPU_node_begin(name);
	totin = 0;
	totout = 0;

	if(in) {
		for(i = 0; in[i].type != GPU_NONE; i++) {
			GPU_node_input(node, in[i].type, in[i].name, in[i].vec, in[i].link, 0);
			totin++;
		}
	}
	
	if(out) {
		for(i = 0; out[i].type != GPU_NONE; i++) {
			GPU_node_output(node, out[i].type, out[i].name, &out[i].link);
			totout++;
		}
	}

	va_start(params, out);
	for(i=0; i<function->totparam; i++) {
		if(function->paramout[i]) {
			if(totout == 0) {
				linkptr= va_arg(params, GPUNodeLink**);
				GPU_node_output(node, function->paramtype[i], "", linkptr);
			}
			else
				totout--;
		}
		else {
			if(totin == 0) {
				link= va_arg(params, GPUNodeLink*);
				gpu_link_input(node, function, i, link);
			}
			else
				totin--;
		}
	}
	va_end(params);

	GPU_node_end(node);

	gpu_material_add_node(mat, node);
	
	return node;
}

