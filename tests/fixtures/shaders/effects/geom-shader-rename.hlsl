// Minimal repro of TranslateGeometryShader.  Exercises three rewrites in
// one fixture: maxvertexcount removal plus the WE_GS_MAX_VERTICES #define,
// the producer-side input rename, and the per-varying gs_in_ array prefix.
[maxvertexcount(1)]
in vec2 v_TexCoord;
void emit() {
    gl_Position = IN[0].gl_Position;
    v_TexCoord = IN[0].v_TexCoord;
    EmitVertex();
}
