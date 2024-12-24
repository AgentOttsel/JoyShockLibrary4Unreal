# JoyShockLibrary4Unreal
This is a fork of JibbSmart's [JoyShockLibrary](https://github.com/JibbSmart/JoyShockLibrary), modified to integrate with Unreal Engine's input system as a plug-in. This allows your Unreal Engine games to support DualShock 4, DualSense (including Edge), Switch Pro and Joy-con controllers natively, and use some of their exclusive features such as gyro and touchpad.

## Installation
- This plug-in requires using microdee's [HIDUE](https://github.com/microdee/HIDUE) plugin for communicating with controllers via USB and Bluetooth. Please add it to your Unreal project first.
- Download or clone the JoyShockLibrary4Unreal repo from this GitHub page and add it to your game's Plugins folder. The path to the Content folder should look like this: `<project>/Plugins/JoyShockLibrary4Unreal/Content`.
- Make sure that both HIDUE and JoyShockLibrary4Unreal are enabled in your project's .uproject file or Plug-in settings.

## Demo Level
- There's a demo level called LV_JoyShockDemo in the Content folder! You can find it in your Content Browser, as long as you enable showing Plugin Content in your Content Browser settings:
![Accessing Test Level](https://github.com/user-attachments/assets/920f87a8-6de6-4efd-a3bd-b8787ea1a9d4)

If you connect a compatible controller and press Play in your editor, you'll be able to see your controller's rotation in real time! It'll probably need calibration the first time you start it. I'll add a shortcut for this in a future update, but for now, you'll have to select the actor called BP_JoyShockVisualizer in the Outliner. This actor contains most of the logic behind this demo level, so feel free to look at its Blueprint later, but for now, go to the Details pane and click the button that says "Start Continuous Calibration" with the controller in a relaxed position, wait a couple of seconds, and then click on "Pause Continuous Calibration".


| Outliner  | Details pane |
| --------- | ------------ |
| ![image](https://github.com/user-attachments/assets/d6903620-1db0-41ff-b6a8-0bc605587bfb)  | ![image](https://github.com/user-attachments/assets/91c26248-ff39-44b4-914b-d810934e5a34) |


The level starts with the JoyShockVisualizer actor in Rotation mode, where it copies your controller's orientation, like in the video below.

https://github.com/user-attachments/assets/dfb27481-388e-4639-affd-2a887db05d33

In order to have this type of 1-to-1 rotation mapping in your game like in the video above, you should get the current Orientation from the JSL4U Get Motion State function:

![JSL4U Get Motion State](https://github.com/user-attachments/assets/5779b093-e757-466a-947e-c96adbd9a6b5)


Another mode you can enable is Aim Mode, by clicking the "Toggle Aim Mode" button in the Details pane (with the BP_JoyShockVisualizer actor selected, just like for calibration). This is a showcase of gyro aiming, and lets you move the controller on a 2D plane:

https://github.com/user-attachments/assets/de8e5e04-ed70-45ba-8188-43ad402b6117

In order to add Gyro aiming like this to your game, get the Y and Z values from the JSL4U Get And Flush Accumulated Gyro function, and apply those inputs to your player's aim:

![Get and flush Accumulated Gyro](https://github.com/user-attachments/assets/b032ec0e-9ebb-4d9c-bd2b-773eec098fcd)

## Input events

For inputs that have an XInput equivalent (e.g. face buttons, triggers and sticks), simply adding JoyShockLibrary4Unreal to your project (following the installation steps below) and enabling it will make Unreal recognize those inputs automatically for any compatible controller, with no code changes required.

For buttons that are exclusive to JoyShock inputs, new input events have been added:
![JoyShock inputs](https://github.com/user-attachments/assets/3ce0e091-e703-4781-823f-33e1aa615997)

## Blueprint nodes

All original JoyShockLibrary functions are still here and exposed to Blueprints. You can quickly find them by searching for the JSL prefix. 

![JSL Functions](https://github.com/user-attachments/assets/08010581-64ec-45c8-9e79-9fc8a2315349)

Additionally, new functions have been created and exposed to Blueprints with the JSL4U prefix:

![JSL4U Functions](https://github.com/user-attachments/assets/1d59f8cd-2888-4ab3-b1ca-2a29b00f3f6b)

JSL4U functions favour Unreal Engine's types and standards, so instead of returning, for example, 3 floats for an acceleration vector, they return an FVector. Additionally, vectors and quaternions were updated to be in Left-handed Z-up, as opposed to Right-handed Y-up.

## Which versions of Unreal Engine has this been tested with?
So far I've used it in Unreal Engine 5.4, but I intend to upgrade my project to 5.5 soon. I expect it to work in other versions of Unreal, but if you find any issues, feel free to let me know.

## Why should I use this plug-in instead of Steam Input or DS4Windows?

They each have their different use cases. Steam Input and DS4Windows do a fantastic job translating non-Xbox controller inputs into XInput, and I love that they can add gyro aiming to games that wouldn't support them otherwise by assigning gyro to Mouse. However, if you're making a game with JoyShockLibrary4Unreal, your game can support those controllers and make use of their features natively, without players having to worry about running background apps or remapping inputs. 

## Can I use the controller motion processors contained in this plugin with official PlayStation and Switch controller libraries?
No official Sony or Nintendo libraries were used in the development or testing of JoyShockLibrary4Unreal, so I'm unable to answer that. However, you are welcome to modify the plug-in as you see fit. The functions in GamepadMotion.hpp should help you process motion data regardless of how you got it.

## Planned future updates
- Improved multiplayer support, especially when mixed with XInput controllers
- Expand test level, with easier calibration, different 3D models for each controller type, and demonstrating more features such as Touch.

## Credits
- A massive thanks to JibbSmart for creating the original JoyShockLibrary plug-in, and for answering the questions I sent to his Twitter DMs. For the full credits of the original JoyShockLibrary, check out his [JoyShockLibrary](https://github.com/JibbSmart/JoyShockLibrary) repo.
- microdee for the [HIDUE](https://github.com/microdee/HIDUE) Unreal plug-in, which JSL4U relies on for both USB and Bluetooth connections.
- DualSense 3D model in the test level created by [Saleem Akhtar](https://www.artstation.com/marketplace/p/zBM9R/ps5-duelsense-controller-3d-model-fbx).
