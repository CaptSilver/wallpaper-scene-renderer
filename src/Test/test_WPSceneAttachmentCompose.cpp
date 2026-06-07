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
#include "AttachmentLinkOrder.hpp"
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

    TEST_CASE("resolved attachment anchors at slot, full local kept") {
        // Uniform rule: world = parent * bone[0].world * att * local.
        // bone[0] of a single-bone flat puppet is at the origin, so the
        // factor reduces to parent * att * local.
        auto puppet = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*puppet, "head", 0, 50.0f, 60.0f, 0.0f);
        Eigen::Matrix4d parent = translation(100.0, 200.0, 0.0);
        Eigen::Matrix4d local  = translation(-72.0, -475.79, 0.0);
        auto r = composeAttachedChildWorld(parent, puppet, "head", local);
        CHECK(r.attachment_resolved);
        // 100 + 50 + (-72) = 78,  200 + 60 + (-475.79) = -215.79
        CHECK(r.world(0, 3) == doctest::Approx(78.0));
        CHECK(r.world(1, 3) == doctest::Approx(-215.79));
    }

    TEST_CASE("resolved attachment preserves scale and rotation in local") {
        auto puppet = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*puppet, "head", 0, 50.0f, 60.0f, 0.0f);
        // Local: translate(999, 999) × rotate(90° Z) × scale(0.5).  Under
        // the uniform rule the full local is composed in, so the world
        // translation reflects all three contributions (translation only
        // for translation-only premultiplied parents).
        Eigen::Affine3d local = Eigen::Affine3d::Identity();
        local.prescale(Eigen::Vector3d(0.5, 0.5, 0.5));
        local.prerotate(Eigen::AngleAxisd(M_PI / 2.0, Eigen::Vector3d::UnitZ()));
        local.pretranslate(Eigen::Vector3d(999.0, 999.0, 0.0));
        Eigen::Matrix4d parent = translation(100.0, 200.0, 0.0);
        auto r = composeAttachedChildWorld(parent, puppet, "head", local.matrix());
        CHECK(r.attachment_resolved);
        // Translation: 100 + 50 + 999, 200 + 60 + 999 (all linears are I).
        CHECK(r.world(0, 3) == doctest::Approx(1149.0));
        CHECK(r.world(1, 3) == doctest::Approx(1259.0));
        // Rotation × scale preserved: Z-90 takes (1,0,0) → (0,1,0); scale 0.5.
        CHECK(r.world(0, 0) == doctest::Approx(0.0).epsilon(1e-6));
        CHECK(r.world(1, 0) == doctest::Approx(0.5).epsilon(1e-6));
    }

    TEST_CASE("two children sharing one attachment slot fan out by their locals") {
        // Under the uniform rule the children differentiate via their
        // authored local offsets — each lands at slot + local.  (Multiple
        // hair strands on one head slot do not pile up at the same point.)
        auto puppet = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*puppet, "hair back", 0, 8.66f, 677.02f, 0.0f);
        Eigen::Matrix4d parent = translation(2170.0, 955.0, 0.0);

        Eigen::Matrix4d local_a = translation(-186.2, -303.4, 0.0);
        Eigen::Matrix4d local_b = translation(-266.3, -265.8, 0.0);

        auto a = composeAttachedChildWorld(parent, puppet, "hair back", local_a);
        auto b = composeAttachedChildWorld(parent, puppet, "hair back", local_b);
        CHECK(a.attachment_resolved);
        CHECK(b.attachment_resolved);
        // a: 2170 + 8.66 + (-186.2) = 1992.46,  955 + 677.02 + (-303.4) = 1328.62
        CHECK(a.world(0, 3) == doctest::Approx(1992.46));
        CHECK(a.world(1, 3) == doctest::Approx(1328.62));
        // b: 2170 + 8.66 + (-266.3) = 1912.36,  955 + 677.02 + (-265.8) = 1366.22
        CHECK(b.world(0, 3) == doctest::Approx(1912.36));
        CHECK(b.world(1, 3) == doctest::Approx(1366.22));
    }

    TEST_CASE("bone[N>0].world_transform lifts attached child by bind-pose offset") {
        // The big new-rule case: when the named attachment rigs to a
        // non-root bone, the child inherits the bone's bind-pose
        // cumulative world transform.  Mirrors SAO's hair-c1 chain
        // where 'head' on asuna body's puppet is bone[2] with a large
        // positive Y offset from bone[0].
        Eigen::Vector3f bone0 { 0.0f, 0.0f, 0.0f };
        Eigen::Vector3f bone1 { 12.5f, -185.0f, 0.0f };
        Eigen::Vector3f bone2 { 77.5f, 252.5f, 0.0f }; // head bone
        auto puppet = makeFlatPuppet({ bone0, bone1, bone2 });
        addAttachment(*puppet, "head", /*bone_index*/ 2, -32.51f, 116.41f, 0.0f);

        Eigen::Matrix4d parent    = translation(2172.5, 1062.4, 0.0);
        Eigen::Matrix4d hairLocal = translation(-72.0, -475.79, 0.0);
        auto r = composeAttachedChildWorld(parent, puppet, "head", hairLocal);
        CHECK(r.attachment_resolved);
        // X: 2172.5 + 77.5 + (-32.51) + (-72.0) = 2145.49
        // Y: 1062.4 + 252.5 + 116.41 + (-475.79) = 955.52
        CHECK(r.world(0, 3) == doctest::Approx(2145.49));
        CHECK(r.world(1, 3) == doctest::Approx(955.52));
    }

    TEST_CASE("3-deep nested chain (SAO Asuna shape, flat skeletons)") {
        // root puppet at scene origin (2170, 955)
        // → child puppet 1 attached to root via 'Attachment'
        //   → child puppet 2 attached to bottom via 'Attachment bottom'
        //     → leaf card attached to body via 'head'
        //
        // All puppets here use single-bone flat skeletons (bone[0]
        // at the origin) — bones[0].world_transform == identity — so
        // the uniform formula reduces to parent × att × local at
        // every step, equivalent to the documented WE pipeline for
        // the bind-pose case.

        auto root       = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*root, "Attachment", 0, -0.15f, -0.10f, 0.0f);

        auto bodyBottom = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*bodyBottom, "Attachment bottom", 0, 2.77f, 103.0f, 0.0f);

        auto body       = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*body, "head", 0, -32.51f, 116.41f, 0.0f);

        Eigen::Matrix4d rootWorld = translation(2170.0, 955.0, 0.0);

        // step 1: body_bottom = root × bones_root[0] × att × bbLocal
        Eigen::Matrix4d bbLocal = translation(-216.0, -209.0, 0.0);
        auto            bb      = composeAttachedChildWorld(rootWorld, root, "Attachment", bbLocal);
        CHECK(bb.attachment_resolved);
        // 2170 + 0 + (-0.15) + (-216) = 1953.85,  955 + 0 + (-0.10) + (-209) = 745.90
        CHECK(bb.world(0, 3) == doctest::Approx(1953.85));
        CHECK(bb.world(1, 3) == doctest::Approx(745.90));

        // step 2: body = bb × bones_bb[0] × att × bLocal
        Eigen::Matrix4d bLocal = translation(-22.3, 192.3, 0.0);
        auto bd = composeAttachedChildWorld(bb.world, bodyBottom, "Attachment bottom", bLocal);
        CHECK(bd.attachment_resolved);
        // 1953.85 + 2.77 + (-22.3) = 1934.32,  745.90 + 103.0 + 192.3 = 1041.20
        CHECK(bd.world(0, 3) == doctest::Approx(1934.32));
        CHECK(bd.world(1, 3) == doctest::Approx(1041.20));

        // step 3: hair = bd × bones_body[0] × att × hairLocal
        Eigen::Matrix4d hairLocal = translation(-72.0, -475.79, 0.0);
        auto h = composeAttachedChildWorld(bd.world, body, "head", hairLocal);
        CHECK(h.attachment_resolved);
        // 1934.32 + (-32.51) + (-72.0) = 1829.81,  1041.20 + 116.41 + (-475.79) = 681.82
        CHECK(h.world(0, 3) == doctest::Approx(1829.81));
        CHECK(h.world(1, 3) == doctest::Approx(681.82));
    }

    TEST_CASE("1-level-deep card on root puppet with zero-offset slot") {
        // Direct child of root puppet (no nesting), bone[0] at origin,
        // attachment at (0,0).  Under the uniform rule the child anchors
        // at puppet + local — its authored offset is fully honored.
        auto root = makeFlatPuppet({ { 0.0f, 0.0f, 0.0f } });
        addAttachment(*root, "Attachment", 0, 0.0f, 0.0f, 0.0f);

        Eigen::Matrix4d puppetWorld = translation(2581.0, 1113.0, 0.0);
        Eigen::Matrix4d childLocal  = translation(24.35, 723.65, 0.0);

        auto r = composeAttachedChildWorld(puppetWorld, root, "Attachment", childLocal);
        CHECK(r.attachment_resolved);
        // 2581 + 0 + 24.35 = 2605.35,  1113 + 0 + 723.65 = 1836.65
        CHECK(r.world(0, 3) == doctest::Approx(2605.35));
        CHECK(r.world(1, 3) == doctest::Approx(1836.65));
    }

    TEST_CASE("dropTranslation helper preserves rotation/scale, zeroes translation") {
        // Helper kept for diagnostic callers even though the compose path
        // no longer drops translation.
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

// The per-frame attachment-proxy refresh must process parents before children
// so a child reads its parent's already-updated world this frame.  Depth = how
// many ancestors (via parent id) are themselves tracked children.  Witness: the
// 3448290956 face chain head(142, plain root from the link set's view) →
// composite(137) → Eyes(157), where 137 tracks 142 and 157 tracks 137.
TEST_SUITE("AttachmentLinkOrder depth") {
    TEST_CASE("root link (parent not a tracked child) is depth 0") {
        // 137 tracks 142, but 142 is not itself a tracked child → depth 0.
        auto d = attachmentLinkDepths({ 137 }, { 142 });
        REQUIRE(d.size() == 1);
        CHECK(d[0] == 0);
    }

    TEST_CASE("nested chain increases depth parent-before-child") {
        // 137←142 (depth 0); 157←137 (137 is tracked → depth 1);
        // 999←157 (157 is tracked, itself depth 1 → depth 2).
        auto d = attachmentLinkDepths({ 137, 157, 999 }, { 142, 137, 157 });
        REQUIRE(d.size() == 3);
        CHECK(d[0] == 0);
        CHECK(d[1] == 1);
        CHECK(d[2] == 2);
    }

    TEST_CASE("siblings sharing a tracked parent are both depth 1") {
        // eyelids 149 and 243 both track composite 137 (depth 0) → both depth 1.
        auto d = attachmentLinkDepths({ 137, 149, 243 }, { 142, 137, 137 });
        REQUIRE(d.size() == 3);
        CHECK(d[0] == 0);
        CHECK(d[1] == 1);
        CHECK(d[2] == 1);
    }

    TEST_CASE("input order independent — child listed before its parent") {
        // Same chain as above but child rows precede the parent row; depths
        // must still reflect the hierarchy, not list position.
        auto d = attachmentLinkDepths({ 999, 157, 137 }, { 157, 137, 142 });
        REQUIRE(d.size() == 3);
        CHECK(d[0] == 2); // 999
        CHECK(d[1] == 1); // 157
        CHECK(d[2] == 0); // 137
    }

    TEST_CASE("cyclic parent chain terminates (no infinite loop)") {
        // Malformed: 1←2 and 2←1.  The seen-guard stops counting on revisit.
        auto d = attachmentLinkDepths({ 1, 2 }, { 2, 1 });
        REQUIRE(d.size() == 2);
        CHECK(d[0] <= 2);
        CHECK(d[1] <= 2);
    }
}
