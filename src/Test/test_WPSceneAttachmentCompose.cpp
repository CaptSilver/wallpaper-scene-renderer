// Integration-style tests for the attached-child world-transform compose
// rule (see WPSceneAttachmentCompose.hpp).
//
// We hand-build a minimal puppet hierarchy that mirrors a 3-deep nested-
// puppet shape (root puppet → puppet → puppet → card) and walk world-
// transform composition top-down, comparing against the documented rule.
// This is integration coverage — it doesn't load scene.pkg from disk
// but exercises the full chain through the same helper that
// WPSceneParser uses.

#include <doctest.h>

#include "WPSceneAttachmentCompose.hpp"
#include "WPPuppet.hpp"

#include <Eigen/Geometry>
#include <memory>

using namespace wallpaper;

namespace
{

// Build a flat-hierarchy puppet (every non-root bone parents bone[0]).
// Each bone's transform is a pure translation taken from the variadic
// list, applied via pretranslate.
std::shared_ptr<WPPuppet> makeFlatPuppet(std::initializer_list<Eigen::Vector3f> bone_translations) {
    auto puppet = std::make_shared<WPPuppet>();
    int  i      = 0;
    for (const auto& t : bone_translations) {
        WPPuppet::Bone b;
        b.parent = (i == 0) ? 0xFFFFFFFFu : 0u;
        b.transform.pretranslate(t);
        puppet->bones.push_back(b);
        ++i;
    }
    puppet->prepared();
    return puppet;
}

// Add a named attachment to the puppet, rigged to bone `bone_index`,
// with a translation-only local matrix.
void addAttachment(WPPuppet& puppet, const std::string& name, uint32_t bone_index,
                   float tx, float ty, float tz) {
    WPPuppet::Attachment att;
    att.name       = name;
    att.bone_index = bone_index;
    att.transform.pretranslate(Eigen::Vector3f(tx, ty, tz));
    puppet.attachments.push_back(std::move(att));
}

// Helper: pure translation as an Eigen::Matrix4d.
Eigen::Matrix4d translation(double x, double y, double z) {
    Eigen::Affine3d a = Eigen::Affine3d::Identity();
    a.pretranslate(Eigen::Vector3d(x, y, z));
    return a.matrix();
}

} // anonymous namespace

TEST_SUITE("WPSceneAttachmentCompose") {
    TEST_CASE("no parent puppet → standard parent × local chain") {
        Eigen::Matrix4d parent = translation(100.0, 200.0, 0.0);
        Eigen::Matrix4d local  = translation(10.0, 5.0, 0.0);
        auto r = composeAttachedChildWorld(parent, /*puppet*/ nullptr, "head", local);
        CHECK_FALSE(r.attachment_resolved);
        CHECK(r.world(0, 3) == doctest::Approx(110.0));
        CHECK(r.world(1, 3) == doctest::Approx(205.0));
    }

    TEST_CASE("empty attachment name → standard chain") {
        auto puppet = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*puppet, "head", 0, 5.0f, 5.0f, 0.0f);
        Eigen::Matrix4d parent = translation(100.0, 200.0, 0.0);
        Eigen::Matrix4d local  = translation(10.0, 5.0, 0.0);
        auto r = composeAttachedChildWorld(parent, puppet, /*name*/ "", local);
        CHECK_FALSE(r.attachment_resolved);
        CHECK(r.world(0, 3) == doctest::Approx(110.0));
        CHECK(r.world(1, 3) == doctest::Approx(205.0));
    }

    TEST_CASE("attachment name not on puppet → standard chain") {
        auto puppet = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*puppet, "head", 0, 5.0f, 5.0f, 0.0f);
        Eigen::Matrix4d parent = translation(100.0, 200.0, 0.0);
        Eigen::Matrix4d local  = translation(10.0, 5.0, 0.0);
        auto r = composeAttachedChildWorld(parent, puppet, "missing", local);
        CHECK_FALSE(r.attachment_resolved);
        CHECK(r.world(0, 3) == doctest::Approx(110.0));
        CHECK(r.world(1, 3) == doctest::Approx(205.0));
    }

    TEST_CASE("attachment with bone_index out of range → standard chain") {
        // Defensive gate: when the named attachment refers to a bone
        // index outside the puppet's skeleton, the attachment has no
        // anchor to live in, so we fall back to the standard chain.
        auto puppet = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } }); // 1 bone
        addAttachment(*puppet, "head", /*bone_index*/ 7, 5.0f, 5.0f, 0.0f);
        Eigen::Matrix4d parent = translation(100.0, 200.0, 0.0);
        Eigen::Matrix4d local  = translation(10.0, 5.0, 0.0);
        auto r = composeAttachedChildWorld(parent, puppet, "head", local);
        CHECK_FALSE(r.attachment_resolved);
        CHECK(r.world(0, 3) == doctest::Approx(110.0));
        CHECK(r.world(1, 3) == doctest::Approx(205.0));
    }

    TEST_CASE("resolved attachment drops local translation, anchors at slot") {
        auto puppet = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*puppet, "head", 0, 50.0f, 60.0f, 0.0f);
        Eigen::Matrix4d parent = translation(100.0, 200.0, 0.0);
        // Authored local has a large negative translation — represents
        // the editor-baked metadata pattern that needs to be neutralized.
        Eigen::Matrix4d local = translation(-72.0, -475.79, 0.0);
        auto r = composeAttachedChildWorld(parent, puppet, "head", local);
        CHECK(r.attachment_resolved);
        // World should be parent + attachment, with local translation
        // discarded.
        CHECK(r.world(0, 3) == doctest::Approx(150.0));
        CHECK(r.world(1, 3) == doctest::Approx(260.0));
    }

    TEST_CASE("resolved attachment preserves scale and rotation in local") {
        auto puppet = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*puppet, "head", 0, 50.0f, 60.0f, 0.0f);
        // Local: rotate 90° around Z, scale 0.5, translate (999, 999) — the
        // translation should be dropped, the rotation+scale kept.
        Eigen::Affine3d local = Eigen::Affine3d::Identity();
        local.prescale(Eigen::Vector3d(0.5, 0.5, 0.5));
        local.prerotate(Eigen::AngleAxisd(M_PI / 2.0, Eigen::Vector3d::UnitZ()));
        local.pretranslate(Eigen::Vector3d(999.0, 999.0, 0.0)); // dropped
        Eigen::Matrix4d parent = translation(100.0, 200.0, 0.0);
        auto r = composeAttachedChildWorld(parent, puppet, "head", local.matrix());
        CHECK(r.attachment_resolved);
        // Translation: parent + attachment (no contribution from local.translation)
        CHECK(r.world(0, 3) == doctest::Approx(150.0));
        CHECK(r.world(1, 3) == doctest::Approx(260.0));
        // Rotation column 0 should reflect the 90° Z rotation × scale 0.5:
        // R*S col0 = (0, 0.5, 0) for Z-90.  parent has no rotation, so the
        // composed result preserves this.
        CHECK(r.world(0, 0) == doctest::Approx(0.0).epsilon(1e-6));
        CHECK(r.world(1, 0) == doctest::Approx(0.5).epsilon(1e-6));
    }

    TEST_CASE("two children sharing one attachment slot land at same anchor") {
        // When two children share the same attachment slot on the same
        // parent, the drop-translation rule places them at exactly the
        // slot's world position even though their authored locals differ.
        // Their texture content is what differentiates the two layers
        // visually.
        auto puppet = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*puppet, "hair back", 0, 8.66f, 677.02f, 0.0f);
        Eigen::Matrix4d parent = translation(2170.0, 955.0, 0.0);

        Eigen::Matrix4d local_a = translation(-186.2, -303.4, 0.0);
        Eigen::Matrix4d local_b = translation(-266.3, -265.8, 0.0);

        auto a = composeAttachedChildWorld(parent, puppet, "hair back", local_a);
        auto b = composeAttachedChildWorld(parent, puppet, "hair back", local_b);
        CHECK(a.attachment_resolved);
        CHECK(b.attachment_resolved);
        CHECK(a.world(0, 3) == doctest::Approx(b.world(0, 3)));
        CHECK(a.world(1, 3) == doctest::Approx(b.world(1, 3)));
        CHECK(a.world(0, 3) == doctest::Approx(2178.66));
        CHECK(a.world(1, 3) == doctest::Approx(1632.02));
    }

    TEST_CASE("3-deep nested chain") {
        // root puppet at scene origin (2170, 955)
        // → child puppet 1 attached to root via 'Attachment'
        //   → child puppet 2 attached to bottom via 'Attachment bottom'
        //     → leaf card attached to body via 'head'
        //
        // We compose top-down and verify each step's world matches the
        // documented rule.

        auto root       = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*root, "Attachment", 0, -0.15f, -0.10f, 0.0f);

        auto bodyBottom = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*bodyBottom, "Attachment bottom", 0, 2.77f, 103.0f, 0.0f);

        auto body       = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*body, "head", 0, -32.51f, 116.41f, 0.0f);

        Eigen::Matrix4d rootWorld = translation(2170.0, 955.0, 0.0);

        // step 1: body_bottom attaches to root@'Attachment', authored local (-216, -209)
        Eigen::Matrix4d bbLocal = translation(-216.0, -209.0, 0.0);
        auto            bb      = composeAttachedChildWorld(rootWorld, root, "Attachment", bbLocal);
        CHECK(bb.attachment_resolved);
        // Drop-translation rule: world = root + att, no contribution from bbLocal.
        CHECK(bb.world(0, 3) == doctest::Approx(2169.85));
        CHECK(bb.world(1, 3) == doctest::Approx(954.90));

        // step 2: body attaches to body_bottom@'Attachment bottom'
        Eigen::Matrix4d bLocal = translation(-22.3, 192.3, 0.0);
        auto bd = composeAttachedChildWorld(bb.world, bodyBottom, "Attachment bottom", bLocal);
        CHECK(bd.attachment_resolved);
        CHECK(bd.world(0, 3) == doctest::Approx(2172.62));
        CHECK(bd.world(1, 3) == doctest::Approx(1057.90));

        // step 3: hair card attaches to body@'head'.  Authored local Y is a
        // huge negative (-475.79) — would drop the card to the feet under
        // a naive `parent × att × local` composition.  Drop-translation
        // pins it to the head slot.
        Eigen::Matrix4d hairLocal = translation(-72.0, -475.79, 0.0);
        auto h = composeAttachedChildWorld(bd.world, body, "head", hairLocal);
        CHECK(h.attachment_resolved);
        CHECK(h.world(0, 3) == doctest::Approx(2140.11));
        CHECK(h.world(1, 3) == doctest::Approx(1174.31));
    }

    TEST_CASE("1-level-deep card on root puppet with zero-offset slot") {
        // Direct child of root puppet (no nesting).  The puppet has a
        // single 'Attachment' slot with zero offset.  Under drop-
        // translation, the child anchors at the puppet origin — its
        // authored local translation is dropped.  This is regression-
        // shaped: pins current behavior so future changes that intend
        // to honor local must explicitly update this expectation.
        auto root = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*root, "Attachment", 0, 0.0f, 0.0f, 0.0f);

        Eigen::Matrix4d puppetWorld = translation(2581.0, 1113.0, 0.0);
        Eigen::Matrix4d childLocal  = translation(24.35, 723.65, 0.0);

        auto r = composeAttachedChildWorld(puppetWorld, root, "Attachment", childLocal);
        CHECK(r.attachment_resolved);
        CHECK(r.world(0, 3) == doctest::Approx(2581.0));
        CHECK(r.world(1, 3) == doctest::Approx(1113.0));
    }

    TEST_CASE("dropTranslation helper preserves rotation/scale, zeroes translation") {
        Eigen::Affine3d a = Eigen::Affine3d::Identity();
        a.prescale(Eigen::Vector3d(2.0, 0.5, 1.0));
        a.prerotate(Eigen::AngleAxisd(M_PI / 4.0, Eigen::Vector3d::UnitZ()));
        a.pretranslate(Eigen::Vector3d(7.0, 8.0, 9.0));
        Eigen::Matrix4d m  = a.matrix();
        Eigen::Matrix4d dt = dropTranslation(m);
        CHECK(dt(0, 3) == doctest::Approx(0.0));
        CHECK(dt(1, 3) == doctest::Approx(0.0));
        CHECK(dt(2, 3) == doctest::Approx(0.0));
        // Upper-3x3 block unchanged.
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                CHECK(dt(r, c) == doctest::Approx(m(r, c)));
        // Last row preserved (homogeneous form).
        CHECK(dt(3, 0) == doctest::Approx(m(3, 0)));
        CHECK(dt(3, 1) == doctest::Approx(m(3, 1)));
        CHECK(dt(3, 2) == doctest::Approx(m(3, 2)));
        CHECK(dt(3, 3) == doctest::Approx(m(3, 3)));
    }
}
