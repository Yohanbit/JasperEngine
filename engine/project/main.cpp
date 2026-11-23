#include "JasperEngine.h"
using namespace jasperengine;

#include "Image.h"

Scene scene;
Image image(&scene);

JASPERENGINE_INIT void init(){
    // The project's Device Tier settings, as an export's generated main.cpp writes them
    // (editor/Generator.cpp, Project Settings > Quality > Device Tiers). Without this call the
    // tier the GPU detects is logged and then not applied (Device.cpp: applyTier() needs a
    // configured project), so a phone classified Low still sets up for every shader of a High
    // project: on a Mali-G76 the first frame took over 20 s and the game never left the splash.
    // "{}" is a project with the defaults; the detected tier's overrides then apply.
    Device::configure(R"JEDEVICE({})JEDEVICE");

    image.setAnchorPreset(AnchorPreset::CENTER);
    image.setTexture("jasperengine.png");

    Engine::setScalingMode(Scaling::FITWIDTH);
    Engine::setCanvasSize(1000,480);
    Engine::setScene(&scene);
}
