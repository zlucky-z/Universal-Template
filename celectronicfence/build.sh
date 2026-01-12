g++ -std=c++11 main.cc -o server -lpthread




g++ -std=c++11 test.cpp -pthread -o test



g++ -std=c++11 -I/opt/sophon/sophon-ffmpeg_1.6.0/include -L/opt/sophon/sophon-ffmpeg_1.6.0/lib main.cc -o server -lpthread -lavformat -lavcodec -lavutil



g++ -std=c++11 delete-test.cpp -o delete-test -lpthread