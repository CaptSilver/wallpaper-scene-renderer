// Minimal repro of FixCombineAlpha.  Combine shaders (godrays, shine, ...)
// add the effect alpha to the base alpha; that extends the alpha bound and
// creates a translucent fringe at puppet edges.  Fix: comment out the
// additive line so the base alpha is preserved.
void main() {
    albedo.a = (clamp(albedo.a + rays.a, 0.0, 1.0));
    glOutColor = albedo;
}
