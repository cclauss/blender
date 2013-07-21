/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2013 Blender Foundation.
 * All rights reserved.
 *
 * Original Author: Joshua Leung
 * Contributor(s): None Yet
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 * Defines and code for core node types
 */
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "BLI_blenlib.h"
#include "BLI_ghash.h"
#include "BLI_utildefines.h"

#include "BKE_depsgraph.h"

#include "RNA_access.h"
#include "RNA_types.h"

#include "depsgraph_types.h"
#include "depsgraph_intern.h"

/* ******************************************************** */
/* Outer Nodes */

/* ID Node ================================================ */

/* Add 'id' node to graph */
static void dnti_outer_id__add_to_graph(Depsgraph *graph, DepsNode *node, ID *id)
{
	/* add to toplevel node and graph */
	BLI_ghash_insert(graph->nodehash, id, node);
	BLI_addtail(graph->nodes, node);
}

/* Group Node ============================================= */

/* Add 'group' node to graph */
static void dnti_outer_group__add_to_graph(Depsgraph *graph, DepsNode *node, ID *id)
{
	
}

/* Data Node ============================================== */

/* Add 'data' node to graph */
static void dnti_data__add_to_graph(Depsgraph *graph, DepsNode *node, ID *id)
{
	DepsNode *id_node;
	
	/* find parent for this node */
	id_node = DEG_find_node(graph, DEPSNODE_TYPE_OUTER_ID, id, NULL, NULL);
	BLI_assert(id_node != NULL);
	
	/* attach to owner */
	node->owner = id_node;
	
	if (id_node->type == DEPSNODE_TYPE_OUTER_ID) {
		IDDepsNode *id_data = (IDDepsNode *)id_node;
		
		/* ID Node - data node is "subdata" here... */
		BLI_addtail(&id_data->subdata, node);
	}
	else {
		GroupDepsNode *grp_data = (GroupDepsNode *)id_node;
		
		/* Group Node */
		// XXX: for quicker checks, it may be nice to be able to have "ID + data" subdata node hash?
		BLI_addtail(&grp_data->subdaa, node);
	}
}

/* ******************************************************** */
/* Inner Nodes */

/* ******************************************************** */
/* Internal API */

/* Make a group from the two given outer nodes */
DepsNode *DEG_group_cyclic_nodes(Depsgraph *graph, DepsNode *node1, DepsNode *node2)
{
	// TODO...
	return NULL;
}

/* ******************************************************** */
/* External API */

/* Global type registry */

/* NOTE: For now, this is a hashtable not array, since the core node types
 * currently do not have contiguous ID values. Using a hash here gives us
 * more flexibility, albeit using more memory and also sacrificing a little
 * speed. Later on, when things stabilise we may turn this back to an array
 * since there are only just a few node types that an array would cope fine...
 */
static GHash *_depsnode_typeinfo_registry = NULL;

/* Registration ------------------------------------------- */

/* Register node type */
void DEG_register_node_typeinfo(DepsNodeTypeInfo *typeinfo)
{
	if (typeinfo) {
		eDepsNode_Type type = typeinfo->type;
		BLI_ghash_insert(_depsnode_typeinfo_registry, SET_INT_IN_POINTER(type), typeinfo);
	}
}


/* Register all node types */
void DEG_register_node_types(void)
{
	/* initialise registry */
	_depsnode_typeinfo_registry = BLI_ghash_int_new(__func__);
	
	/* register node types */
	DEG_register_node_typeinfo(DNTI_OUTER_ID);
	DEG_register_node_typeinfo(DNTI_OUTER_GROUP);
	DEG_register_node_typeinfo(DNTI_OUTER_OP);
	
	DEG_register_node_typeinfo(DNTI_DATA);
	
	// ...
}

/* Free registry on exit */
void DEG_free_node_types(void)
{
	BLI_ghash_free(_depsnode_typeinfo_registry, NULL, NULL);
}

/* Getters ------------------------------------------------- */

/* Get typeinfo for specified type */
DepsNodeTypeInfo *DEG_get_typeinfo(eDepsNode_Type type)
{
	DepsNodeTypeInfo *nti = NULL;
	
	// TODO: look up typeinfo associated with this type...
	return nti;
}

/* Get typeinfo for provided node */
DepsNodeTypeInfo *DEG_node_get_typeinfo(DepsNode *node)
{
	DepsNodeTypeInfo *nti = NULL;
	
	if (node) {
		nti = DEG_get_typeinfo(node->type);
	}
	return nti;
}

/* ******************************************************** */
