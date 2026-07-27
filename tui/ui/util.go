package ui

import "fmt"

func humanSize(n int64) string {
	f := float64(n)
	units := []string{"B", "K", "M", "G", "T"}
	for i, unit := range units {
		if f < 1024 || i == len(units)-1 {
			if unit == "B" {
				return fmt.Sprintf("%.0f%s", f, unit)
			}
			return fmt.Sprintf("%.1f%s", f, unit)
		}
		f /= 1024
	}
	return fmt.Sprintf("%.1fP", f)
}
