#!/bin/bash
# input is the scenario file

gnuplot << EOF
# Note you need gnuplot 4.4 for the pdfcairo terminal.

set terminal pdfcairo font "Gill Sans,10" linewidth 2 rounded

# Line style for axes
set style line 80 lt rgb "#808080"

# Line style for grid
set style line 81 lt 0  # dashed
set style line 81 lt rgb "#808080"  # grey

set grid back linestyle 81
set border 3 back linestyle 80 # Remove border on top and right.  These
             # borders are useless and make it harder
             # to see plotted lines near the border.
    # Also, put it in grey; no need for so much emphasis on a border.
set xtics nomirror
set ytics nomirror

#set log x
#set mxtics 10    # Makes logscale look good.

# Line styles: try to pick pleasing colors, rather
# than strictly primary colors or hard-to-see colors
# like gnuplot's default yellow.  Make the lines thick
# so they're easy to see in small plots in papers.
set style line 1 lt rgb "#A00000" lw 1 pt 1
set style line 2 lt rgb "#00A000" lw 1 pt 6
set style line 3 lt rgb "#5060D0" lw 1 pt 2
set style line 4 lt rgb "#F25900" lw 1 pt 9


#set boxwidth 0.9 absolute
#set style fill   solid 1.00 border lt -1

#set style data histogram
#set style histogram cluster gap 1

# set xtics 1,1,6

set output "ledbat.pdf"

set ylabel 'Delay (ms.)'
set xlabel 'Time'

set yrange [1:15000] reverse

set key bottom left


plot 'ledbat' u 1:2 t "ping pong" smooth bezier, '' u 1:3 t "slow start" smooth bezier, '' u 1:4 t "aimd" smooth bezier, '' u 1:5 t "keep alive" smooth bezier, '' u 1:6 t "close" smooth bezier

#, '' u 1:7 t "reset"

EOF



