
uniform mat4 ModelViewMatrix;
uniform mat4 ModelViewProjectionMatrix;
uniform mat4 ModelMatrix;

in vec3 pos;

out vec4 pPos;
out vec3 vPos;

void main()
{
	vPos = (ModelViewMatrix * vec4(pos, 1.0)).xyz;
	pPos = ModelViewProjectionMatrix * vec4(pos, 1.0);
	/* Small bias to always be on top of the geom. */
	pPos.z -= 1e-3;

#ifdef USE_WORLD_CLIP_PLANES
	world_clip_planes_calc_clip_distance((ModelMatrix * vec4(pos, 1.0)).xyz);
#endif
}
