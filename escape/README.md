things to do when copying template:
1. in `template\build_MSVC.cmd`
    * replace `template` with the name of your folder in `..\template\build\%CMAKE_BUILD_TYPE%\game.exe`
2. in `game.cpp`
    * replace `template` with the name of your folder in `m_ffmpegWriter = std::make_unique<FFmpegWriter>(extent.width, extent.height, "C:/Users/aanny/source/repos/fork/template/videos/recording.h264");`

to convert h264 to mp4:
`ffmpeg -i recording.h264 -c:v copy output.mp4`

to start receiver.cpp:
`g++ receiver.cpp -o receiver.exe ^
  -IC:/ffmpeg/include -IC:/SDL3/x86_64-w64-mingw32/include ^
  -LC:/ffmpeg/lib -LC:/SDL3/x86_64-w64-mingw32/lib ^
  -lavcodec -lavutil -lswscale -lSDL3 -lws2_32`

  cd C:\Users\aanny\source\repos\fork