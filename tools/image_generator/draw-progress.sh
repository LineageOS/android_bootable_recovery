#!/bin/bash

# arc central angle in degrees
arc_size="64.5"

arc_start=$(bc -l <<< "90 - $arc_size / 2")
arc_end=$(bc -l <<< "90 + $arc_size / 2")

N=100
for ((i=0; i < $N; i++)); do
	progress=$(bc -l <<< "$i / ($N - 1)")
	fg_arc_start=$(bc -l <<< "$arc_end - $progress * $arc_size")

	filename="progress$(printf "%02d" $i).png"
	echo "-- Writing file: $filename"

	convert -size 400x400 xc:black \
		-draw "stroke-linecap round stroke-width 8 \
				stroke gray ellipse 200,200 100,100 $arc_start,$arc_end \
				stroke white ellipse 200,200 100,100 $fg_arc_start,$arc_end" "$filename"

  echo "-- Writing file: rtl_$filename"
  convert -size 400x400 xc:black \
  		-draw "stroke-linecap round stroke-width 8 \
  				stroke gray ellipse 200,200 100,100 $arc_start,$arc_end \
  				stroke white ellipse 200,200 100,100 $fg_arc_start,$arc_end" "rtl_$filename"

		mogrify -crop 120x30+140+280 "$filename"
		mogrify -crop 120x30+140+280 "rtl_$filename"

		# Use color format recovery can use
		mogrify -define png:format=png24 -type TrueColor "$filename"
    mogrify -define png:format=png24 -type TrueColor "rtl_$filename"

		mogrify -flop "rtl_$filename"
done
