#include <doctest/doctest.h>

#include "core/nr_model_plan.hpp"

using namespace stray_dlss::nrmodel_plan;

TEST_CASE("scale 1.0 is OFF: the present stage behaves exactly as before")
{
	Config c;
	c.scale = 1.0f;
	const Plan p = plan(c, 3840, 2160, true, true);
	CHECK(p.result == Result::off);
	CHECK(p.width == 0);
	CHECK(p.height == 0);
}

TEST_CASE("scale 0.5 on 4K puts the model at exactly 1920x1080 - the guide extent")
{
	Config c;
	c.scale = 0.5f;
	const Plan p = plan(c, 3840, 2160, true, true);
	CHECK(p.result == Result::ok);
	CHECK(p.width == 1920);
	CHECK(p.height == 1080);
	CHECK(p.transfer_strength == 1.0f);
}

TEST_CASE("the small extent is even, and 1440p at 0.7 rounds sensibly")
{
	Config c;
	c.scale = 0.7f;
	const Plan p = plan(c, 2560, 1440, true, true);
	CHECK(p.result == Result::ok);
	CHECK(p.width % 2 == 0);
	CHECK(p.height % 2 == 0);
	CHECK(p.width == 1792);
	CHECK(p.height == 1008);
}

TEST_CASE("the resolve needs a typed UAV on the frame's format: refused without one")
{
	Config c;
	c.scale = 0.5f;
	CHECK(plan(c, 3840, 2160, false, true).result == Result::no_typed_uav);
	CHECK(plan(c, 3840, 2160, true, false).result == Result::no_typed_uav);
}

TEST_CASE("a scale that leaves the model nothing to see is refused, not honoured")
{
	Config c;
	c.scale = 0.1f;
	CHECK(plan(c, 3840, 2160, true, true).result == Result::bad_scale);
	c.scale = 0.25f;
	CHECK(plan(c, 3840, 2160, true, true).result == Result::ok);
	c.scale = 0.3f;
	CHECK(plan(c, 200, 120, true, true).result == Result::too_small);
}

TEST_CASE("transfer strength is clamped at zero from below and passed through above")
{
	Config c;
	c.scale = 0.5f;
	c.transfer_strength = -1.0f;
	CHECK(plan(c, 3840, 2160, true, true).transfer_strength == 0.0f);
	c.transfer_strength = 1.5f;
	CHECK(plan(c, 3840, 2160, true, true).transfer_strength == 1.5f);
}

TEST_CASE("the resolve mode is clamped and carries the one parameter it uses")
{
	Config c;
	c.scale = 0.5f;
	c.guided = 7;
	c.guided_sigma = 0.2f;
	c.guided_epsilon = 0.005f;
	Plan p = plan(c, 3840, 2160, true, true);
	CHECK(p.guided == 2);
	CHECK(p.guided_param == 0.005f);   // mode 2 carries epsilon
	c.guided = 1;
	p = plan(c, 3840, 2160, true, true);
	CHECK(p.guided == 1);
	CHECK(p.guided_param == 0.2f);     // mode 1 carries sigma
	c.guided = -3;
	p = plan(c, 3840, 2160, true, true);
	CHECK(p.guided == 0);
	// Degenerate parameters are floored, never zero: both divide by them in the shader.
	c.guided = 2;
	c.guided_epsilon = 0.0f;
	CHECK(plan(c, 3840, 2160, true, true).guided_param > 0.0f);
	c.guided = 1;
	c.guided_sigma = -1.0f;
	CHECK(plan(c, 3840, 2160, true, true).guided_param > 0.0f);
}

TEST_CASE("every result has a name")
{
	for (int i = 0; i < kResultCount; ++i)
	{
		const char *n = result_name(static_cast<Result>(i));
		CHECK(n != nullptr);
		CHECK(n[0] != '?');
	}
}
