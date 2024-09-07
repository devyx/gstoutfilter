Gstreamer output filter based on DistroAV code.

Intended to stream filtered source over network to other PC/R-PI/OBS.

Tested pipeline in the filter (you need to change IP and port of the target or use multicast address)

```
glupload ! glcolorconvert ! gldownload ! queue ! video/x-raw,format=NV12 ! vaapih264enc quality-level=7 prediction-type=1 cpb-length=100 keyframe-period=1 bitrate=10000 !  h264parse ! rtph264pay config-interval=1 ! udpsink host=127.0.0.1 port=5000
```

Playback using a gst-launch (sync=false on the final element is important):
```
gst-launch-1.0 udpsrc port=5001 ! application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,packetization-mode=1,profile=high,payload=96 ! rtph264depay ! h264parse ! vah264dec ! videoconvert ! autovideosink sync=false
```

