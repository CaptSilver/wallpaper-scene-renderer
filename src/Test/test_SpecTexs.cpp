#include <doctest.h>

#include "SpecTexs.hpp"

using namespace wallpaper;

TEST_SUITE("SpecTexs") {
    TEST_CASE("IsSpecTex") {
        CHECK(IsSpecTex("_rt_default") == true);
        CHECK(IsSpecTex("_rt_link_42") == true);
        CHECK(IsSpecTex("_rt_offscreen_5") == true);
        CHECK(IsSpecTex("_rt_") == true);
        CHECK(IsSpecTex("mytex") == false);
        CHECK(IsSpecTex("_r") == false);
        CHECK(IsSpecTex("") == false);
    }

    TEST_CASE("IsSpecLinkTex") {
        CHECK(IsSpecLinkTex("_rt_link_42") == true);
        CHECK(IsSpecLinkTex("_rt_link_0") == true);
        CHECK(IsSpecLinkTex("_rt_link_") == true);
        CHECK(IsSpecLinkTex("_rt_default") == false);
        CHECK(IsSpecLinkTex("mytex") == false);
        CHECK(IsSpecLinkTex("") == false);
    }

    TEST_CASE("ParseLinkTex") {
        CHECK(ParseLinkTex("_rt_link_42") == 42u);
        CHECK(ParseLinkTex("_rt_link_0") == 0u);
        CHECK(ParseLinkTex("_rt_link_100") == 100u);
    }

    TEST_CASE("GenLinkTex") {
        CHECK(GenLinkTex(0) == "_rt_link_0");
        CHECK(GenLinkTex(42) == "_rt_link_42");
        CHECK(GenLinkTex(100) == "_rt_link_100");
    }

    TEST_CASE("GenOffscreenRT") {
        CHECK(GenOffscreenRT(0) == "_rt_offscreen_0");
        CHECK(GenOffscreenRT(5) == "_rt_offscreen_5");
        CHECK(GenOffscreenRT(99) == "_rt_offscreen_99");
    }

    TEST_CASE("GenLinkTex / ParseLinkTex round-trip") {
        for (uint32_t i = 0; i < 20; i++) {
            auto tex = GenLinkTex(static_cast<idx>(i));
            CHECK(IsSpecLinkTex(tex));
            CHECK(ParseLinkTex(tex) == i);
        }
    }

    TEST_CASE("WE_SCENE_DEPTH constant is _rt_sceneDepth and IsSpecTex-true") {
        CHECK(WE_SCENE_DEPTH == "_rt_sceneDepth");
        CHECK(IsSpecTex(WE_SCENE_DEPTH) == true);
    }

    // The media system-texture names are plain '$'-prefixed keys, not
    // '_rt_'-prefixed render targets -- LoadMaterial's ParseSpecTexName call
    // must stay a no-op for them, so IsSpecTex has to read false here.
    TEST_CASE("media system-texture names are IsSpecTex-false") {
        CHECK(WE_MEDIA_THUMBNAIL == "$mediaThumbnail");
        CHECK(WE_MEDIA_PREVIOUS_THUMBNAIL == "$mediaPreviousThumbnail");
        CHECK(IsSpecTex(WE_MEDIA_THUMBNAIL) == false);
        CHECK(IsSpecTex(WE_MEDIA_PREVIOUS_THUMBNAIL) == false);
    }

} // TEST_SUITE

TEST_SUITE("SpecTexs_Volumetrics") {
    TEST_CASE("WE_VOLUMETRICS_BACK is _rt_volumetricsBack and IsSpecTex-true") {
        CHECK(WE_VOLUMETRICS_BACK == std::string_view("_rt_volumetricsBack"));
        CHECK(IsSpecTex(WE_VOLUMETRICS_BACK));
    }
    TEST_CASE("WE_VOLUMETRICS_LIGHT_BUFFER is _rt_volumetricsLightBuffer") {
        CHECK(WE_VOLUMETRICS_LIGHT_BUFFER == std::string_view("_rt_volumetricsLightBuffer"));
        CHECK(IsSpecTex(WE_VOLUMETRICS_LIGHT_BUFFER));
    }
    TEST_CASE("WE_VOLUMETRICS_LIGHT_BUFFER_B is _rt_volumetricsLightBufferB") {
        CHECK(WE_VOLUMETRICS_LIGHT_BUFFER_B == std::string_view("_rt_volumetricsLightBufferB"));
        CHECK(IsSpecTex(WE_VOLUMETRICS_LIGHT_BUFFER_B));
    }
    TEST_CASE("WE_VOLUMETRICS_SINGLE is _rt_volumetricsSingle and IsSpecTex-true") {
        CHECK(WE_VOLUMETRICS_SINGLE == std::string_view("_rt_volumetricsSingle"));
        CHECK(IsSpecTex(WE_VOLUMETRICS_SINGLE));
    }
} // SpecTexs_Volumetrics

TEST_SUITE("SpecTexs_OwnCompositeChainInput") {
    TEST_CASE("the layer's own composite A in slot 0 means the chain input") {
        CHECK(IsOwnCompositeChainInput("_rt_imageLayerComposite_140_a", 140, false));
        CHECK(IsOwnCompositeChainInput("_rt_imageLayerComposite_140_a", 140, true));
        CHECK(IsOwnCompositeChainInput("_rt_imageLayerComposite_140", 140, false));
    }

    TEST_CASE("own composite B means the chain input only without a static source") {
        // With a static source texture the _b route samples that instead.
        CHECK(IsOwnCompositeChainInput("_rt_imageLayerComposite_18_b", 18, false));
        CHECK_FALSE(IsOwnCompositeChainInput("_rt_imageLayerComposite_424_b", 424, true));
    }

    TEST_CASE("another layer's composite is a cross-layer link, not the chain input") {
        CHECK_FALSE(IsOwnCompositeChainInput("_rt_imageLayerComposite_141_a", 140, false));
        CHECK_FALSE(IsOwnCompositeChainInput("_rt_imageLayerComposite_1400_a", 140, false));
        CHECK_FALSE(IsOwnCompositeChainInput("_rt_imageLayerComposite_14_a", 140, false));
    }

    TEST_CASE("other names are never the chain input") {
        CHECK_FALSE(IsOwnCompositeChainInput("", 140, false));
        CHECK_FALSE(IsOwnCompositeChainInput("_rt_FullFrameBuffer", 140, false));
        CHECK_FALSE(IsOwnCompositeChainInput("_rt_imageLayerComposite_140_c", 140, false));
        CHECK_FALSE(IsOwnCompositeChainInput("_rt_imageLayerComposite_x_a", 140, false));
    }
}

TEST_SUITE("SpecTexs_GlTexNames") {
    TEST_CASE("texture uniform names carry their suffix on every slot") {
        CHECK(std::string_view(WE_GLTEX_NAMES[0]) == "g_Texture0");
        CHECK(std::string_view(WE_GLTEX_NAMES[12]) == "g_Texture12");
        CHECK(std::string_view(WE_GLTEX_RESOLUTION_NAMES[3]) == "g_Texture3Resolution");
        CHECK(std::string_view(WE_GLTEX_ROTATION_NAMES[0]) == "g_Texture0Rotation");
        CHECK(std::string_view(WE_GLTEX_TRANSLATION_NAMES[10]) == "g_Texture10Translation");
        CHECK(std::string_view(WE_GLTEX_MIPMAPINFO_NAMES[12]) == "g_Texture12MipMapInfo");
        CHECK(WE_GLTEX_NAMES.size() == 13u);
    }
}
