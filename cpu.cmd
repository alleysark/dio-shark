set title "I/O statistics per cpu"
set key outside
#set style histogram cluster gap 1
set style histogram rowstacked
set style data histogram
set style fill solid
set boxwidth 0.75

set xlabel "CPU"
set ylabel "No. of I/O"

plot 'dioparse.cpu.dat' using 2:xtic(1) title "read" with histograms linecolor rgb 'dark-blue', '' using 3 title "write" with histograms linecolor rgb 'forest-green'

