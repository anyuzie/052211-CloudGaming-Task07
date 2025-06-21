# task 7 updates
* added imgui to game.cpp, such that users are able to see all objects in the scene. achieved through using tags to retrieve only objects that are tagged, to prevent repeated display of the same scene objects in different forms
* imgui window can also control the update of rotation, position and colour of spot light; position and colour of point light; and colour of directional light
* changes are reflected on the receiver

# setup
to run the game, download the game into the repository containing Vienna Vulkan Engine.
* make sure that the `escape`, `receiver.cpp`, `SDL3.dll` and `stb_image_write.h` are on the same repository as Vienna Vulkan Engine

to start receiver.cpp:
`g++ receiver.cpp -o receiver.exe ^
  -IC:/ffmpeg/include -IC:/SDL3/x86_64-w64-mingw32/include ^
  -LC:/ffmpeg/lib -LC:/SDL3/x86_64-w64-mingw32/lib ^
  -lavcodec -lavutil -lswscale -lSDL3 -lws2_32`

to convert h264 to mp4:
`ffmpeg -i recording.h264 -c:v copy output.mp4`
