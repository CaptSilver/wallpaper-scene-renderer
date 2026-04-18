#include <doctest.h>

#include "Particle/Particle.h"
#include "Particle/ParticleEmitter.h"
#include "Particle/ParticleModify.h"
#include "Particle/ParticleSystem.h"

#include <cmath>

using namespace wallpaper;

namespace
{
// Helper: create a default particle with known state
Particle makeParticle() {
    Particle p;
    p.position = Eigen::Vector3f(1.0f, 2.0f, 3.0f);
    p.velocity = Eigen::Vector3f(0.5f, 1.0f, 1.5f);
    p.color    = Eigen::Vector3f(1.0f, 1.0f, 1.0f);
    p.alpha    = 1.0f;
    p.size     = 20.0f;
    p.lifetime = 1.0f;
    p.rotation = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
    p.angularVelocity = Eigen::Vector3f(0.1f, 0.2f, 0.3f);
    p.mark_new = true;
    p.init.lifetime = 1.0f;
    p.init.alpha    = 1.0f;
    p.init.size     = 20.0f;
    p.init.color    = Eigen::Vector3f(1.0f, 1.0f, 1.0f);
    return p;
}
} // namespace

// ===========================================================================
// Move
// ===========================================================================

TEST_SUITE("ParticleModify_Move") {

TEST_CASE("Move adds delta to position") {
    Particle p = makeParticle();
    ParticleModify::Move(p, Eigen::Vector3d(1.0, 2.0, 3.0));
    CHECK(p.position.x() == doctest::Approx(2.0f));
    CHECK(p.position.y() == doctest::Approx(4.0f));
    CHECK(p.position.z() == doctest::Approx(6.0f));
}

TEST_CASE("Move with xyz overload") {
    Particle p = makeParticle();
    ParticleModify::Move(p, -1.0, 0.0, 1.0);
    CHECK(p.position.x() == doctest::Approx(0.0f));
    CHECK(p.position.y() == doctest::Approx(2.0f));
    CHECK(p.position.z() == doctest::Approx(4.0f));
}

} // TEST_SUITE

// ===========================================================================
// MoveTo
// ===========================================================================

TEST_SUITE("ParticleModify_MoveTo") {

TEST_CASE("MoveTo sets position absolutely") {
    Particle p = makeParticle();
    ParticleModify::MoveTo(p, Eigen::Vector3d(10.0, 20.0, 30.0));
    CHECK(p.position.x() == doctest::Approx(10.0f));
    CHECK(p.position.y() == doctest::Approx(20.0f));
    CHECK(p.position.z() == doctest::Approx(30.0f));
}

} // TEST_SUITE

// ===========================================================================
// MoveToNegZ
// ===========================================================================

TEST_SUITE("ParticleModify_MoveToNegZ") {

TEST_CASE("MoveToNegZ negates positive Z") {
    Particle p = makeParticle();
    p.position.z() = 5.0f;
    ParticleModify::MoveToNegZ(p);
    CHECK(p.position.z() == doctest::Approx(-5.0f));
}

TEST_CASE("MoveToNegZ keeps already negative Z") {
    Particle p = makeParticle();
    p.position.z() = -3.0f;
    ParticleModify::MoveToNegZ(p);
    CHECK(p.position.z() == doctest::Approx(-3.0f));
}

} // TEST_SUITE

// ===========================================================================
// MoveByTime
// ===========================================================================

TEST_SUITE("ParticleModify_MoveByTime") {

TEST_CASE("MoveByTime applies velocity times time") {
    Particle p = makeParticle();
    p.position = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
    p.velocity = Eigen::Vector3f(2.0f, 4.0f, 6.0f);
    ParticleModify::MoveByTime(p, 0.5);
    CHECK(p.position.x() == doctest::Approx(1.0f));
    CHECK(p.position.y() == doctest::Approx(2.0f));
    CHECK(p.position.z() == doctest::Approx(3.0f));
}

} // TEST_SUITE

// ===========================================================================
// MoveMultiply
// ===========================================================================

TEST_SUITE("ParticleModify_MoveMultiply") {

TEST_CASE("MoveMultiply component-wise multiply") {
    Particle p = makeParticle();
    p.position = Eigen::Vector3f(2.0f, 3.0f, 4.0f);
    ParticleModify::MoveMultiply(p, Eigen::Vector3d(0.5, 2.0, 1.0));
    CHECK(p.position.x() == doctest::Approx(1.0f));
    CHECK(p.position.y() == doctest::Approx(6.0f));
    CHECK(p.position.z() == doctest::Approx(4.0f));
}

} // TEST_SUITE

// ===========================================================================
// MoveApplySign
// ===========================================================================

TEST_SUITE("ParticleModify_MoveApplySign") {

TEST_CASE("MoveApplySign applies signs") {
    Particle p = makeParticle();
    p.position = Eigen::Vector3f(3.0f, -4.0f, 5.0f);
    ParticleModify::MoveApplySign(p, -1, 1, -1);
    CHECK(p.position.x() == doctest::Approx(-3.0f));
    CHECK(p.position.y() == doctest::Approx(4.0f));
    CHECK(p.position.z() == doctest::Approx(-5.0f));
}

TEST_CASE("MoveApplySign zero means no change") {
    Particle p = makeParticle();
    p.position = Eigen::Vector3f(3.0f, -4.0f, 5.0f);
    ParticleModify::MoveApplySign(p, 0, 0, 0);
    CHECK(p.position.x() == doctest::Approx(3.0f));
    CHECK(p.position.y() == doctest::Approx(-4.0f));
    CHECK(p.position.z() == doctest::Approx(5.0f));
}

TEST_CASE("MoveApplySign positive sign preserves abs value") {
    // Kills mul_to_div: abs(2)*1 = 2, but abs(2)/1 = 2 (same for 1)
    // Use value != 1 to distinguish: abs(3)*2 = 6, abs(3)/2 = 1.5
    Particle p = makeParticle();
    p.position = Eigen::Vector3f(-3.0f, -4.0f, -5.0f);
    ParticleModify::MoveApplySign(p, 2, 2, 2);
    CHECK(p.position.x() == doctest::Approx(6.0f));
    CHECK(p.position.y() == doctest::Approx(8.0f));
    CHECK(p.position.z() == doctest::Approx(10.0f));
}

TEST_CASE("MoveApplySign negative sign with magnitude") {
    Particle p = makeParticle();
    p.position = Eigen::Vector3f(3.0f, 4.0f, 5.0f);
    ParticleModify::MoveApplySign(p, -2, -3, -1);
    CHECK(p.position.x() == doctest::Approx(-6.0f));
    CHECK(p.position.y() == doctest::Approx(-12.0f));
    CHECK(p.position.z() == doctest::Approx(-5.0f));
}

} // TEST_SUITE

// ===========================================================================
// Lifetime
// ===========================================================================

TEST_SUITE("ParticleModify_Lifetime") {

TEST_CASE("InitLifetime sets both current and init") {
    Particle p = makeParticle();
    ParticleModify::InitLifetime(p, 5.0f);
    CHECK(p.lifetime == doctest::Approx(5.0f));
    CHECK(p.init.lifetime == doctest::Approx(5.0f));
}

TEST_CASE("LifetimePos at start is 0 percent") {
    Particle p = makeParticle();
    ParticleModify::InitLifetime(p, 2.0f);
    double pos = ParticleModify::LifetimePos(p);
    CHECK(pos == doctest::Approx(0.0));
}

TEST_CASE("LifetimePos at midpoint is 50 percent") {
    Particle p = makeParticle();
    ParticleModify::InitLifetime(p, 2.0f);
    ParticleModify::ChangeLifetime(p, -1.0);
    double pos = ParticleModify::LifetimePos(p);
    CHECK(pos == doctest::Approx(0.5));
}

TEST_CASE("LifetimePos at zero lifetime returns 1.0") {
    // Kills lt_to_le: lifetime==0 should NOT enter the < 0 branch
    Particle p = makeParticle();
    p.lifetime = 0.0f;
    p.init.lifetime = 2.0f;
    double pos = ParticleModify::LifetimePos(p);
    CHECK(pos == doctest::Approx(1.0));
}

TEST_CASE("LifetimePos negative lifetime returns 1.0") {
    Particle p = makeParticle();
    p.lifetime = -1.0f;
    p.init.lifetime = 2.0f;
    CHECK(ParticleModify::LifetimePos(p) == doctest::Approx(1.0));
}

TEST_CASE("LifetimePassed returns elapsed time") {
    Particle p = makeParticle();
    ParticleModify::InitLifetime(p, 3.0f);
    ParticleModify::ChangeLifetime(p, -1.0);
    CHECK(ParticleModify::LifetimePassed(p) == doctest::Approx(1.0));
}

TEST_CASE("LifetimeOk alive") {
    Particle p = makeParticle();
    p.lifetime = 1.0f;
    CHECK(ParticleModify::LifetimeOk(p) == true);
}

TEST_CASE("LifetimeOk dead") {
    Particle p = makeParticle();
    p.lifetime = 0.0f;
    CHECK(ParticleModify::LifetimeOk(p) == false);
}

} // TEST_SUITE

// ===========================================================================
// Velocity + Acceleration
// ===========================================================================

TEST_SUITE("ParticleModify_Velocity") {

TEST_CASE("ChangeVelocity adds delta") {
    Particle p = makeParticle();
    p.velocity = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    ParticleModify::ChangeVelocity(p, Eigen::Vector3d(0.0, 1.0, 0.0));
    CHECK(p.velocity.x() == doctest::Approx(1.0f));
    CHECK(p.velocity.y() == doctest::Approx(1.0f));
    CHECK(p.velocity.z() == doctest::Approx(0.0f));
}

TEST_CASE("Accelerate adds acc times time") {
    Particle p = makeParticle();
    p.velocity = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
    ParticleModify::Accelerate(p, Eigen::Vector3d(0.0, -9.8, 0.0), 0.5);
    CHECK(p.velocity.x() == doctest::Approx(0.0f));
    CHECK(p.velocity.y() == doctest::Approx(-4.9f));
    CHECK(p.velocity.z() == doctest::Approx(0.0f));
}

} // TEST_SUITE

// ===========================================================================
// Color
// ===========================================================================

TEST_SUITE("ParticleModify_Color") {

TEST_CASE("ChangeColor additive") {
    Particle p = makeParticle();
    p.color = Eigen::Vector3f(0.5f, 0.5f, 0.5f);
    ParticleModify::ChangeColor(p, Eigen::Vector3d(0.1, 0.2, 0.3));
    CHECK(p.color.x() == doctest::Approx(0.6f));
    CHECK(p.color.y() == doctest::Approx(0.7f));
    CHECK(p.color.z() == doctest::Approx(0.8f));
}

TEST_CASE("MutiplyColor multiplicative") {
    Particle p = makeParticle();
    p.color = Eigen::Vector3f(0.5f, 0.8f, 1.0f);
    ParticleModify::MutiplyColor(p, Eigen::Vector3d(2.0, 0.5, 1.0));
    CHECK(p.color.x() == doctest::Approx(1.0f));
    CHECK(p.color.y() == doctest::Approx(0.4f));
    CHECK(p.color.z() == doctest::Approx(1.0f));
}

TEST_CASE("InitColor sets both current and init") {
    Particle p = makeParticle();
    ParticleModify::InitColor(p, 0.2, 0.4, 0.6);
    CHECK(p.color.x() == doctest::Approx(0.2f));
    CHECK(p.color.y() == doctest::Approx(0.4f));
    CHECK(p.color.z() == doctest::Approx(0.6f));
    CHECK(p.init.color.x() == doctest::Approx(0.2f));
    CHECK(p.init.color.y() == doctest::Approx(0.4f));
    CHECK(p.init.color.z() == doctest::Approx(0.6f));
}

} // TEST_SUITE

// ===========================================================================
// Alpha + Size
// ===========================================================================

TEST_SUITE("ParticleModify_AlphaSize") {

TEST_CASE("MutiplyAlpha") {
    Particle p = makeParticle();
    p.alpha = 0.8f;
    ParticleModify::MutiplyAlpha(p, 0.5);
    CHECK(p.alpha == doctest::Approx(0.4f));
}

TEST_CASE("MutiplySize") {
    Particle p = makeParticle();
    p.size = 10.0f;
    ParticleModify::MutiplySize(p, 3.0);
    CHECK(p.size == doctest::Approx(30.0f));
}

TEST_CASE("InitSize sets both current and init") {
    Particle p = makeParticle();
    ParticleModify::InitSize(p, 42.0);
    CHECK(p.size == doctest::Approx(42.0f));
    CHECK(p.init.size == doctest::Approx(42.0f));
}

TEST_CASE("InitAlpha sets both current and init") {
    Particle p = makeParticle();
    ParticleModify::InitAlpha(p, 0.7);
    CHECK(p.alpha == doctest::Approx(0.7f));
    CHECK(p.init.alpha == doctest::Approx(0.7f));
}

TEST_CASE("ChangeSize additive") {
    Particle p = makeParticle();
    p.size = 10.0f;
    ParticleModify::ChangeSize(p, 5.0);
    CHECK(p.size == doctest::Approx(15.0f));
}

TEST_CASE("ChangeAlpha additive") {
    Particle p = makeParticle();
    p.alpha = 0.5f;
    ParticleModify::ChangeAlpha(p, 0.3);
    CHECK(p.alpha == doctest::Approx(0.8f));
}

} // TEST_SUITE

// ===========================================================================
// Init multipliers
// ===========================================================================

TEST_SUITE("ParticleModify_InitMultiply") {

TEST_CASE("MutiplyInitLifeTime multiplies and saves to init") {
    Particle p = makeParticle();
    ParticleModify::InitLifetime(p, 2.0f);
    ParticleModify::MutiplyInitLifeTime(p, 0.5);
    CHECK(p.lifetime == doctest::Approx(1.0f));
    CHECK(p.init.lifetime == doctest::Approx(1.0f));
}

TEST_CASE("MutiplyInitAlpha multiplies and saves to init") {
    Particle p = makeParticle();
    ParticleModify::InitAlpha(p, 0.8);
    ParticleModify::MutiplyInitAlpha(p, 0.5);
    CHECK(p.alpha == doctest::Approx(0.4f));
    CHECK(p.init.alpha == doctest::Approx(0.4f));
}

TEST_CASE("MutiplyInitSize multiplies and saves to init") {
    Particle p = makeParticle();
    ParticleModify::InitSize(p, 10.0);
    ParticleModify::MutiplyInitSize(p, 3.0);
    CHECK(p.size == doctest::Approx(30.0f));
    CHECK(p.init.size == doctest::Approx(30.0f));
}

TEST_CASE("MutiplyInitColor multiplies and saves to init") {
    Particle p = makeParticle();
    ParticleModify::InitColor(p, 0.5, 0.8, 1.0);
    ParticleModify::MutiplyInitColor(p, 2.0, 0.5, 1.0);
    CHECK(p.color.x() == doctest::Approx(1.0f));
    CHECK(p.color.y() == doctest::Approx(0.4f));
    CHECK(p.color.z() == doctest::Approx(1.0f));
    CHECK(p.init.color.x() == doctest::Approx(1.0f));
    CHECK(p.init.color.y() == doctest::Approx(0.4f));
    CHECK(p.init.color.z() == doctest::Approx(1.0f));
}

} // TEST_SUITE

// ===========================================================================
// Reset
// ===========================================================================

TEST_SUITE("ParticleModify_Reset") {

TEST_CASE("Reset restores from init values") {
    Particle p = makeParticle();
    ParticleModify::InitAlpha(p, 0.9);
    ParticleModify::InitSize(p, 15.0);
    ParticleModify::InitColor(p, 0.3, 0.6, 0.9);

    // Modify current values
    p.alpha = 0.1f;
    p.size  = 100.0f;
    p.color = Eigen::Vector3f(0.0f, 0.0f, 0.0f);

    ParticleModify::Reset(p);
    CHECK(p.alpha == doctest::Approx(0.9f));
    CHECK(p.size == doctest::Approx(15.0f));
    CHECK(p.color.x() == doctest::Approx(0.3f));
    CHECK(p.color.y() == doctest::Approx(0.6f));
    CHECK(p.color.z() == doctest::Approx(0.9f));
}

} // TEST_SUITE

// ===========================================================================
// MarkOld + IsNew
// ===========================================================================

TEST_SUITE("ParticleModify_NewOld") {

TEST_CASE("new particle IsNew true") {
    Particle p = makeParticle();
    CHECK(ParticleModify::IsNew(p) == true);
}

TEST_CASE("MarkOld then IsNew false") {
    Particle p = makeParticle();
    ParticleModify::MarkOld(p);
    CHECK(ParticleModify::IsNew(p) == false);
}

} // TEST_SUITE

// ===========================================================================
// Rotation
// ===========================================================================

TEST_SUITE("ParticleModify_Rotation") {

TEST_CASE("RotateByTime applies angular velocity times time") {
    Particle p = makeParticle();
    p.rotation = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
    p.angularVelocity = Eigen::Vector3f(1.0f, 2.0f, 3.0f);
    ParticleModify::RotateByTime(p, 0.5);
    CHECK(p.rotation.x() == doctest::Approx(0.5f));
    CHECK(p.rotation.y() == doctest::Approx(1.0f));
    CHECK(p.rotation.z() == doctest::Approx(1.5f));
}

TEST_CASE("Rotate adds delta") {
    Particle p = makeParticle();
    p.rotation = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    ParticleModify::Rotate(p, Eigen::Vector3d(0.0, 1.0, 0.0));
    CHECK(p.rotation.x() == doctest::Approx(1.0f));
    CHECK(p.rotation.y() == doctest::Approx(1.0f));
    CHECK(p.rotation.z() == doctest::Approx(0.0f));
}

} // TEST_SUITE

// ===========================================================================
// Getters + InitVelocity/InitPos
// ===========================================================================

TEST_SUITE("ParticleModify_Getters") {

TEST_CASE("GetPos returns position reference") {
    Particle p = makeParticle();
    const auto& pos = ParticleModify::GetPos(p);
    CHECK(pos.x() == doctest::Approx(1.0f));
    CHECK(pos.y() == doctest::Approx(2.0f));
    CHECK(pos.z() == doctest::Approx(3.0f));
}

TEST_CASE("GetVelocity returns velocity reference") {
    Particle p = makeParticle();
    const auto& vel = ParticleModify::GetVelocity(p);
    CHECK(vel.x() == doctest::Approx(0.5f));
    CHECK(vel.y() == doctest::Approx(1.0f));
    CHECK(vel.z() == doctest::Approx(1.5f));
}

TEST_CASE("InitVelocity sets velocity") {
    Particle p = makeParticle();
    ParticleModify::InitVelocity(p, 10.0, 20.0, 30.0);
    CHECK(p.velocity.x() == doctest::Approx(10.0f));
    CHECK(p.velocity.y() == doctest::Approx(20.0f));
    CHECK(p.velocity.z() == doctest::Approx(30.0f));
}

TEST_CASE("InitPos sets position") {
    Particle p = makeParticle();
    ParticleModify::InitPos(p, 7.0, 8.0, 9.0);
    CHECK(p.position.x() == doctest::Approx(7.0f));
    CHECK(p.position.y() == doctest::Approx(8.0f));
    CHECK(p.position.z() == doctest::Approx(9.0f));
}

TEST_CASE("MutiplyVelocity scales velocity") {
    Particle p = makeParticle();
    p.velocity = Eigen::Vector3f(2.0f, 4.0f, 6.0f);
    ParticleModify::MutiplyVelocity(p, 0.5);
    CHECK(p.velocity.x() == doctest::Approx(1.0f));
    CHECK(p.velocity.y() == doctest::Approx(2.0f));
    CHECK(p.velocity.z() == doctest::Approx(3.0f));
}

} // TEST_SUITE

// ===========================================================================
// ControlPointForce — radial force from control point
// Tests the force computation pattern used by the controlpointforce operator.
// ===========================================================================

TEST_SUITE("ControlPointForce") {

namespace
{
// Simulate controlpointforce: radial push away from control point
void applyControlPointForce(std::span<Particle> particles,
                            const Eigen::Vector3d& cpOffset,
                            const Eigen::Vector3d& origin,
                            float scale, float threshold, double dt) {
    Eigen::Vector3d offset = cpOffset + origin;
    for (auto& p : particles) {
        Eigen::Vector3d diff     = ParticleModify::GetPos(p).cast<double>() - offset;
        double          distance = diff.norm();
        if (distance < threshold && distance > 0.0) {
            ParticleModify::Accelerate(p, diff.normalized() * scale, dt);
        }
    }
}

// Simulate controlpointattract: pull toward control point
void applyControlPointAttract(std::span<Particle> particles,
                              const Eigen::Vector3d& cpOffset,
                              const Eigen::Vector3d& origin,
                              float scale, float threshold, double dt) {
    Eigen::Vector3d offset = cpOffset + origin;
    for (auto& p : particles) {
        Eigen::Vector3d diff     = offset - ParticleModify::GetPos(p).cast<double>();
        double          distance = diff.norm();
        if (distance < threshold) {
            ParticleModify::Accelerate(p, diff.normalized() * scale, dt);
        }
    }
}
} // namespace

TEST_CASE("Force pushes particle away from control point") {
    Particle p = makeParticle();
    p.position = Eigen::Vector3f(10.0f, 0.0f, 0.0f);
    p.velocity = Eigen::Vector3f::Zero();

    Eigen::Vector3d cpOffset(0, 0, 0);
    Eigen::Vector3d origin(0, 0, 0);
    applyControlPointForce({ &p, 1 }, cpOffset, origin, 100.0f, 512.0f, 1.0);

    // Velocity should be in +x direction (away from CP at origin)
    CHECK(p.velocity.x() > 0.0f);
    CHECK(p.velocity.y() == doctest::Approx(0.0f));
    CHECK(p.velocity.z() == doctest::Approx(0.0f));
}

TEST_CASE("Attract pulls particle toward control point") {
    Particle p = makeParticle();
    p.position = Eigen::Vector3f(10.0f, 0.0f, 0.0f);
    p.velocity = Eigen::Vector3f::Zero();

    Eigen::Vector3d cpOffset(0, 0, 0);
    Eigen::Vector3d origin(0, 0, 0);
    applyControlPointAttract({ &p, 1 }, cpOffset, origin, 100.0f, 512.0f, 1.0);

    // Velocity should be in -x direction (toward CP at origin)
    CHECK(p.velocity.x() < 0.0f);
    CHECK(p.velocity.y() == doctest::Approx(0.0f));
    CHECK(p.velocity.z() == doctest::Approx(0.0f));
}

TEST_CASE("Threshold blocks distant particles") {
    Particle p = makeParticle();
    p.position = Eigen::Vector3f(100.0f, 0.0f, 0.0f);
    p.velocity = Eigen::Vector3f::Zero();

    Eigen::Vector3d cpOffset(0, 0, 0);
    Eigen::Vector3d origin(0, 0, 0);
    // Threshold is 50, particle is at distance 100 — no force
    applyControlPointForce({ &p, 1 }, cpOffset, origin, 100.0f, 50.0f, 1.0);

    CHECK(p.velocity.x() == doctest::Approx(0.0f));
    CHECK(p.velocity.y() == doctest::Approx(0.0f));
    CHECK(p.velocity.z() == doctest::Approx(0.0f));
}

TEST_CASE("Threshold allows near particles") {
    Particle p = makeParticle();
    p.position = Eigen::Vector3f(10.0f, 0.0f, 0.0f);
    p.velocity = Eigen::Vector3f::Zero();

    Eigen::Vector3d cpOffset(0, 0, 0);
    Eigen::Vector3d origin(0, 0, 0);
    // Threshold is 50, particle is at distance 10 — force applied
    applyControlPointForce({ &p, 1 }, cpOffset, origin, 100.0f, 50.0f, 1.0);

    CHECK(p.velocity.x() == doctest::Approx(100.0f));
}

TEST_CASE("Scale affects force magnitude") {
    Particle p1 = makeParticle();
    p1.position = Eigen::Vector3f(10.0f, 0.0f, 0.0f);
    p1.velocity = Eigen::Vector3f::Zero();

    Particle p2 = p1;

    Eigen::Vector3d cpOffset(0, 0, 0);
    Eigen::Vector3d origin(0, 0, 0);
    applyControlPointForce({ &p1, 1 }, cpOffset, origin, 100.0f, 512.0f, 1.0);
    applyControlPointForce({ &p2, 1 }, cpOffset, origin, 200.0f, 512.0f, 1.0);

    CHECK(p2.velocity.x() == doctest::Approx(p1.velocity.x() * 2.0f));
}

TEST_CASE("Origin offset shifts effective control point") {
    Particle p = makeParticle();
    p.position = Eigen::Vector3f(10.0f, 0.0f, 0.0f);
    p.velocity = Eigen::Vector3f::Zero();

    Eigen::Vector3d cpOffset(0, 0, 0);
    // Origin offset puts the effective CP at (10, 0, 0) — same as particle
    Eigen::Vector3d origin(10.0, 0.0, 0.0);
    applyControlPointForce({ &p, 1 }, cpOffset, origin, 100.0f, 512.0f, 1.0);

    // Zero distance → no force (distance > 0.0 guard)
    CHECK(p.velocity.x() == doctest::Approx(0.0f));
    CHECK(p.velocity.y() == doctest::Approx(0.0f));
    CHECK(p.velocity.z() == doctest::Approx(0.0f));
}

TEST_CASE("Zero distance is a no-op") {
    Particle p = makeParticle();
    p.position = Eigen::Vector3f(0.0f, 0.0f, 0.0f);
    p.velocity = Eigen::Vector3f::Zero();

    Eigen::Vector3d cpOffset(0, 0, 0);
    Eigen::Vector3d origin(0, 0, 0);
    applyControlPointForce({ &p, 1 }, cpOffset, origin, 100.0f, 512.0f, 1.0);

    // Particle at CP — no defined direction, should not change
    CHECK(p.velocity.x() == doctest::Approx(0.0f));
    CHECK(p.velocity.y() == doctest::Approx(0.0f));
    CHECK(p.velocity.z() == doctest::Approx(0.0f));
}

TEST_CASE("3D diagonal force direction") {
    Particle p = makeParticle();
    p.position = Eigen::Vector3f(1.0f, 1.0f, 1.0f);
    p.velocity = Eigen::Vector3f::Zero();

    Eigen::Vector3d cpOffset(0, 0, 0);
    Eigen::Vector3d origin(0, 0, 0);
    applyControlPointForce({ &p, 1 }, cpOffset, origin, 100.0f, 512.0f, 1.0);

    // Force direction is normalized (1,1,1) = (1/√3, 1/√3, 1/√3)
    // Velocity = direction * scale * dt = (1/√3 * 100, ...)
    float expected = 100.0f / std::sqrt(3.0f);
    CHECK(p.velocity.x() == doctest::Approx(expected).epsilon(0.001));
    CHECK(p.velocity.y() == doctest::Approx(expected).epsilon(0.001));
    CHECK(p.velocity.z() == doctest::Approx(expected).epsilon(0.001));
}

} // TEST_SUITE

// ===========================================================================
// EmitterDuration — emitter lifetime limit wrapper
// Tests the duration wrapper pattern used by genParticleEmittOp.
// ===========================================================================

TEST_SUITE("EmitterDuration") {

namespace
{
// A mock emitter op that counts how many times it was called.
struct MockEmitter {
    int callCount = 0;
    ParticleEmittOp makeOp() {
        return [this](std::vector<Particle>&, std::vector<ParticleInitOp>&, uint32_t, double) {
            callCount++;
        };
    }
};

// Replicate the duration wrapper logic from genParticleEmittOp
ParticleEmittOp wrapWithDuration(ParticleEmittOp baseOp, float duration) {
    if (duration <= 0.0f) return baseOp;
    return [duration, elapsed = 0.0, baseOp = std::move(baseOp)](
               std::vector<Particle>& ps, std::vector<ParticleInitOp>& inis, uint32_t maxcount,
               double timepass) mutable {
        elapsed += timepass;
        if (elapsed <= duration) {
            baseOp(ps, inis, maxcount, timepass);
        }
    };
}
} // namespace

TEST_CASE("Duration zero means unlimited") {
    MockEmitter mock;
    auto op = wrapWithDuration(mock.makeOp(), 0.0f);
    std::vector<Particle>       ps;
    std::vector<ParticleInitOp> inis;
    op(ps, inis, 100, 1.0);
    op(ps, inis, 100, 1.0);
    op(ps, inis, 100, 1.0);
    CHECK(mock.callCount == 3);
}

TEST_CASE("Emitter active within duration") {
    MockEmitter mock;
    auto op = wrapWithDuration(mock.makeOp(), 5.0f);
    std::vector<Particle>       ps;
    std::vector<ParticleInitOp> inis;
    op(ps, inis, 100, 2.0);
    op(ps, inis, 100, 2.0);
    CHECK(mock.callCount == 2);
}

TEST_CASE("Emitter stops after duration expires") {
    MockEmitter mock;
    auto op = wrapWithDuration(mock.makeOp(), 3.0f);
    std::vector<Particle>       ps;
    std::vector<ParticleInitOp> inis;
    op(ps, inis, 100, 1.0); // elapsed=1
    op(ps, inis, 100, 1.0); // elapsed=2
    op(ps, inis, 100, 1.0); // elapsed=3, == duration, emits
    op(ps, inis, 100, 1.0); // elapsed=4, past
    op(ps, inis, 100, 1.0); // elapsed=5, past
    CHECK(mock.callCount == 3);
}

TEST_CASE("Boundary frame at exact duration still emits") {
    MockEmitter mock;
    auto op = wrapWithDuration(mock.makeOp(), 2.0f);
    std::vector<Particle>       ps;
    std::vector<ParticleInitOp> inis;
    op(ps, inis, 100, 2.0); // elapsed=2.0, == duration
    CHECK(mock.callCount == 1);
    op(ps, inis, 100, 0.001); // elapsed=2.001, past
    CHECK(mock.callCount == 1);
}

TEST_CASE("Duration permanently stops emitter") {
    MockEmitter mock;
    auto op = wrapWithDuration(mock.makeOp(), 1.0f);
    std::vector<Particle>       ps;
    std::vector<ParticleInitOp> inis;
    op(ps, inis, 100, 0.5); // elapsed=0.5
    op(ps, inis, 100, 0.5); // elapsed=1.0, boundary
    op(ps, inis, 100, 0.5); // elapsed=1.5, past
    CHECK(mock.callCount == 2);
    for (int i = 0; i < 100; i++) {
        op(ps, inis, 100, 1.0);
    }
    CHECK(mock.callCount == 2);
}

TEST_CASE("Small time steps accumulate correctly") {
    MockEmitter mock;
    auto op = wrapWithDuration(mock.makeOp(), 0.1f);
    std::vector<Particle>       ps;
    std::vector<ParticleInitOp> inis;
    for (int i = 0; i < 10; i++) {
        op(ps, inis, 100, 0.01);
    }
    CHECK(mock.callCount == 10);
    op(ps, inis, 100, 0.01);
    CHECK(mock.callCount == 10);
}

TEST_CASE("Large time step exceeds duration on first call") {
    MockEmitter mock;
    auto op = wrapWithDuration(mock.makeOp(), 1.0f);
    std::vector<Particle>       ps;
    std::vector<ParticleInitOp> inis;
    op(ps, inis, 100, 100.0); // elapsed=100, way past 1s duration
    CHECK(mock.callCount == 0);
}

} // TEST_SUITE

// ===========================================================================
// ParticleInstance::Refresh — underpins ParticleSubSystem::Reset which the
// dynamic-asset pool uses to rearm spent burst particle FX (e.g. dino_run's
// coinget pickup sparkle).  Without proper refresh, pool-reuse would leave
// dead particles in the instance and the instantaneous emitter would never
// re-fire.
// ===========================================================================

TEST_SUITE("ParticleInstance_Refresh") {

TEST_CASE("Refresh clears particles vector") {
    ParticleInstance inst;
    inst.ParticlesVec().push_back(makeParticle());
    inst.ParticlesVec().push_back(makeParticle());
    REQUIRE(inst.ParticlesVec().size() == 2);

    inst.Refresh();
    CHECK(inst.ParticlesVec().empty());
}

TEST_CASE("Refresh resets death flags to false") {
    ParticleInstance inst;
    inst.SetDeath(true);
    inst.SetNoLiveParticle(true);
    REQUIRE(inst.IsDeath());
    REQUIRE(inst.IsNoLiveParticle());

    inst.Refresh();
    CHECK_FALSE(inst.IsDeath());
    CHECK_FALSE(inst.IsNoLiveParticle());
}

TEST_CASE("Refresh clears bounded-data link") {
    ParticleInstance parent;
    ParticleInstance child;
    child.GetBoundedData().parent       = &parent;
    child.GetBoundedData().particle_idx = 42;
    REQUIRE(child.GetBoundedData().parent == &parent);

    child.Refresh();
    CHECK(child.GetBoundedData().parent == nullptr);
    CHECK(child.GetBoundedData().particle_idx == -1);
}

TEST_CASE("Refresh clears trail histories") {
    ParticleInstance inst;
    inst.InitTrails(4, 1.0f);
    // Seed a few trail points so Refresh has something to clear
    auto& trails = inst.TrailHistories();
    if (trails.empty()) {
        // Instance doesn't pre-size — allocate one ourselves
        trails.emplace_back();
        trails.back().Init(4, 1.0f);
    }
    ParticleTrailPoint pt;
    pt.position = Eigen::Vector3f(1, 2, 3);
    pt.size     = 1.0f;
    pt.alpha    = 1.0f;
    trails[0].Push(pt);
    REQUIRE(trails[0].Count() == 1);

    inst.Refresh();
    for (auto& trail : inst.TrailHistories()) {
        CHECK(trail.Count() == 0);
    }
}

} // TEST_SUITE
