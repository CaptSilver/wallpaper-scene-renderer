#include <doctest.h>

#include "Particle/Particle.h"
#include "Particle/ParticleModify.h"

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
