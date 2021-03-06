/*
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
 * The Original Code is Copyright (C) Blender Foundation
 * All rights reserved.
 */

/** \file \ingroup modifiers
 */


#ifndef __MOD_FLUIDSIM_UTIL_H__
#define __MOD_FLUIDSIM_UTIL_H__

struct FluidsimModifierData;
struct Mesh;
struct ModifierEvalContext;
struct Object;
struct Scene;

/* new fluid-modifier interface */
void fluidsim_init(struct FluidsimModifierData *fluidmd);
void fluidsim_free(struct FluidsimModifierData *fluidmd);

struct Mesh *fluidsimModifier_do(
        struct FluidsimModifierData *fluidmd,
        const struct ModifierEvalContext *ctx,
        struct Mesh *me);

#endif
