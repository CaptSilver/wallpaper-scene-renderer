// Minimal repro of FixImplicitConversions (vec2 -> vec4 upgrade).
// The author declared a vec2 but reads .z out of it; we need to upgrade the
// declaration AND rewrite the texture() call so the sampler argument keeps
// vec2 type.
vec2 tc;
float z = tc.z;
vec4 s = texture(g_Tex, tc);
