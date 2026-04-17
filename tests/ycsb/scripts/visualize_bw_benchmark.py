#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NUMA Bandwidth Benchmark Visualizer
Reads metrics.csv and generates multi-panel benchmark report.

Usage:
    python3 visualize_bw_benchmark.py --input metrics.csv --output report.png [--show] [--dpi 150]

Output:
    PNG chart file (default 2400x1800px at 150 DPI)
"""

import argparse
import csv
import sys
import os
from datetime import datetime
from pathlib import Path

# ============================================================================
# CSV Parsing Functions
# ============================================================================

def safe_float(s, default=0.0):
    """Safely convert string to float with default value.
    
    Args:
        s: Input string to convert
        default: Default value if conversion fails
    
    Returns:
        Float value or default
    """
    try:
        return float(str(s).strip())
    except (ValueError, AttributeError):
        return default


def parse_csv(filepath):
    """Parse metrics.csv, skip PHASE_MARKER rows, return data dict.
    
    CSV Format:
        timestamp,phase,ops_total,ops_sec,used_mem_mb,rss_mb,frag_ratio,
        migrate_total,migrate_sec,numa_pages_n0,numa_pages_n1,evicted_keys
        
    PHASE_MARKER rows:
        PHASE_MARKER,phase_num,phase_name,timestamp
    
    Args:
        filepath: Path to the CSV file
        
    Returns:
        tuple: (data_dict, phase_markers, first_timestamp)
            - data_dict: Dictionary with all metric arrays
            - phase_markers: List of (relative_sec, phase_num, phase_name)
            - first_timestamp: First Unix timestamp in the data
    """
    data = {
        'time': [],           # Relative time in seconds
        'phase': [],          # Phase name (e.g., '1_fill')
        'ops_sec': [],        # Operations per second
        'used_mem_mb': [],    # Used memory in MB
        'rss_mb': [],         # RSS memory in MB
        'frag_ratio': [],     # Memory fragmentation ratio
        'migrate_total': [],  # Total migrations
        'migrate_sec': [],    # Migrations per second
        'numa_pages_n0': [],  # NUMA Node0 page access rate
        'numa_pages_n1': [],  # NUMA Node1 page access rate
        'evicted_keys': []    # Cumulative evicted keys count
    }
    phase_markers = []  # [(relative_sec, phase_num, phase_name)]
    
    first_ts = None
    
    try:
        with open(filepath, 'r', newline='', encoding='utf-8') as f:
            reader = csv.reader(f)
            
            # Skip header
            try:
                header = next(reader)
            except StopIteration:
                print("ERROR: Empty CSV file")
                return data, phase_markers, None
            
            for row in reader:
                # Skip empty rows
                if not row or len(row) == 0:
                    continue
                
                # Handle PHASE_MARKER rows
                if row[0] == 'PHASE_MARKER':
                    try:
                        if len(row) >= 4:
                            marker_ts = int(row[3])
                            phase_num = row[1]
                            phase_name = row[2]
                            phase_markers.append((marker_ts, phase_num, phase_name))
                    except (ValueError, IndexError):
                        pass
                    continue
                
                # Parse data rows
                try:
                    ts = int(row[0])
                    if first_ts is None:
                        first_ts = ts
                    rel_time = ts - first_ts
                    
                    data['time'].append(rel_time)
                    data['phase'].append(row[1] if len(row) > 1 else '')
                    data['ops_sec'].append(safe_float(row[3] if len(row) > 3 else 0))
                    data['used_mem_mb'].append(safe_float(row[4] if len(row) > 4 else 0))
                    data['rss_mb'].append(safe_float(row[5] if len(row) > 5 else 0))
                    data['frag_ratio'].append(safe_float(row[6] if len(row) > 6 else 0))
                    data['migrate_total'].append(safe_float(row[7] if len(row) > 7 else 0))
                    data['migrate_sec'].append(safe_float(row[8] if len(row) > 8 else 0))
                    data['numa_pages_n0'].append(safe_float(row[9] if len(row) > 9 else 0))
                    data['numa_pages_n1'].append(safe_float(row[10] if len(row) > 10 else 0))
                    data['evicted_keys'].append(safe_float(row[11] if len(row) > 11 else 0))
                except (ValueError, IndexError) as e:
                    # Skip malformed rows
                    continue
    
    except FileNotFoundError:
        print(f"ERROR: File not found: {filepath}")
        return data, phase_markers, None
    except PermissionError:
        print(f"ERROR: Permission denied: {filepath}")
        return data, phase_markers, None
    except Exception as e:
        print(f"ERROR: Failed to read CSV: {e}")
        return data, phase_markers, None
    
    # Convert phase_markers times to relative times
    markers = []
    for marker_ts, phase_num, phase_name in phase_markers:
        rel_ts = marker_ts - first_ts if first_ts else 0
        markers.append((rel_ts, phase_num, phase_name))
    
    return data, markers, first_ts


# ============================================================================
# Phase Range Extraction
# ============================================================================

def get_phase_ranges(data):
    """Extract time ranges for each phase from data['phase'] column.
    
    Args:
        data: Data dictionary with 'time' and 'phase' arrays
        
    Returns:
        dict: { '1_fill': (start_sec, end_sec), '2_hotspot': ..., '3_sustain': ... }
    """
    phase_ranges = {}
    
    if not data['time'] or not data['phase']:
        return phase_ranges
    
    # Track phase transitions
    current_phase = None
    phase_start = None
    
    for i, (t, phase) in enumerate(zip(data['time'], data['phase'])):
        if phase != current_phase:
            # Save previous phase range
            if current_phase is not None and phase_start is not None:
                phase_ranges[current_phase] = (phase_start, t)
            
            # Start new phase
            current_phase = phase
            phase_start = t
    
    # Save last phase
    if current_phase is not None and phase_start is not None:
        # Use last time point as end
        phase_ranges[current_phase] = (phase_start, data['time'][-1])
    
    return phase_ranges


# ============================================================================
# Visualization Functions
# ============================================================================

def add_phase_background(ax, phase_ranges, max_time):
    """Add background color shading for each phase.
    
    Args:
        ax: Matplotlib axes object
        phase_ranges: Dict of phase name -> (start, end) tuples
        max_time: Maximum time value for plotting
    """
    # Phase color scheme
    colors = {
        '1_fill': '#E3F2FD',      # Light blue
        '2_hotspot': '#FFF9C4',   # Light yellow
        '3_sustain': '#FFEBEE'    # Light red
    }
    
    # Phase labels for annotation
    labels = {
        '1_fill': 'Phase 1: Fill',
        '2_hotspot': 'Phase 2: Hotspot',
        '3_sustain': 'Phase 3: Sustain'
    }
    
    # Track phase boundaries for vertical lines
    phase_boundaries = []
    
    for phase_name, (start, end) in phase_ranges.items():
        color = colors.get(phase_name, '#F5F5F5')
        
        # Add background shading
        ax.axvspan(start, end, alpha=0.3, color=color, zorder=0)
        
        # Record boundaries (skip first phase start)
        if start > 0:
            phase_boundaries.append((start, labels.get(phase_name, phase_name)))
    
    # Draw vertical dashed lines at phase boundaries
    for boundary, label in phase_boundaries:
        ax.axvline(x=boundary, color='gray', linestyle='--', 
                   linewidth=1.0, alpha=0.7, zorder=1)
    
    # Add phase labels at top of plot
    for phase_name, (start, end) in phase_ranges.items():
        mid = (start + end) / 2
        label = labels.get(phase_name, phase_name)
        # Get y-axis limits for positioning
        ylim = ax.get_ylim()
        y_pos = ylim[1] - (ylim[1] - ylim[0]) * 0.05
        ax.text(mid, y_pos, label, ha='center', va='top', 
                fontsize=7, alpha=0.7, zorder=5)


def plot_report(data, markers, first_ts, output_path, dpi=150, show=False):
    """Generate the multi-panel benchmark report.
    
    Args:
        data: Data dictionary with all metrics
        markers: Phase markers list
        first_ts: First Unix timestamp
        output_path: Output PNG file path
        dpi: DPI for output image
        show: Whether to show interactive plot window
    """
    import matplotlib
    matplotlib.use('Agg')  # Non-interactive backend (no GUI required)
    import matplotlib.pyplot as plt
    import matplotlib.patches as mpatches
    
    # Validate data
    if not data['time']:
        print("ERROR: No data points to plot")
        return False
    
    # Create figure with 3x2 subplots
    fig, axes = plt.subplots(3, 2, figsize=(16, 12))
    
    # Main title with timestamp
    if first_ts:
        start_time_str = datetime.fromtimestamp(first_ts).strftime("%Y-%m-%d %H:%M:%S")
    else:
        start_time_str = "Unknown"
    
    fig.suptitle(
        f'NUMA Bandwidth Benchmark Report\n'
        f'Started: {start_time_str}',
        fontsize=14, fontweight='bold'
    )
    
    # Get phase ranges
    phase_ranges = get_phase_ranges(data)
    max_time = max(data['time']) if data['time'] else 0
    t = data['time']
    
    # ========================================================================
    # [1] Throughput (ops/sec)
    # ========================================================================
    ax = axes[0, 0]
    add_phase_background(ax, phase_ranges, max_time)
    ax.plot(t, data['ops_sec'], color='#1565C0', linewidth=0.8)
    ax.set_title('Throughput', fontsize=11)
    ax.set_ylabel('ops/sec')
    ax.grid(True, alpha=0.3, linestyle='--')
    ax.set_xlim(0, max_time)
    
    # ========================================================================
    # [2] Memory Usage (used_mem + rss)
    # ========================================================================
    ax = axes[0, 1]
    add_phase_background(ax, phase_ranges, max_time)
    ax.plot(t, data['used_mem_mb'], color='#1976D2', linewidth=0.8, label='used_memory')
    ax.plot(t, data['rss_mb'], color='#D32F2F', linewidth=0.8, label='rss')
    
    # Add memory threshold lines
    ax.axhline(y=4096, color='gray', linestyle=':', alpha=0.7, 
               linewidth=1.0, label='Node0 capacity (4GB)')
    ax.axhline(y=3500, color='orange', linestyle=':', alpha=0.7, 
               linewidth=1.0, label='maxmemory (3.5GB)')
    
    ax.set_title('Memory Usage', fontsize=11)
    ax.set_ylabel('MB')
    ax.legend(fontsize=7, loc='best')
    ax.grid(True, alpha=0.3, linestyle='--')
    ax.set_xlim(0, max_time)
    
    # ========================================================================
    # [3] Migration Rate
    # ========================================================================
    ax = axes[1, 0]
    add_phase_background(ax, phase_ranges, max_time)
    ax.plot(t, data['migrate_sec'], color='#7B1FA2', linewidth=0.8)
    ax.set_title('Migration Rate', fontsize=11)
    ax.set_ylabel('migrations/sec')
    ax.grid(True, alpha=0.3, linestyle='--')
    ax.set_xlim(0, max_time)
    
    # ========================================================================
    # [4] NUMA Page Access Rate
    # ========================================================================
    ax = axes[1, 1]
    add_phase_background(ax, phase_ranges, max_time)
    ax.plot(t, data['numa_pages_n0'], color='#388E3C', linewidth=0.8, 
            label='Node 0 (DRAM)')
    ax.plot(t, data['numa_pages_n1'], color='#F57C00', linewidth=0.8, 
            label='Node 1 (CXL)')
    ax.set_title('NUMA Page Access Rate', fontsize=11)
    ax.set_ylabel('pages/sec')
    ax.legend(fontsize=7, loc='best')
    ax.grid(True, alpha=0.3, linestyle='--')
    ax.set_xlim(0, max_time)
    
    # ========================================================================
    # [5] Memory Fragmentation Ratio
    # ========================================================================
    ax = axes[2, 0]
    add_phase_background(ax, phase_ranges, max_time)
    ax.plot(t, data['frag_ratio'], color='#795548', linewidth=0.8)
    
    # Add ideal fragmentation line
    ax.axhline(y=1.0, color='green', linestyle=':', alpha=0.7, 
               linewidth=1.0, label='Ideal (1.0)')
    
    ax.set_title('Memory Fragmentation Ratio', fontsize=11)
    ax.set_ylabel('ratio')
    ax.legend(fontsize=7, loc='best')
    ax.grid(True, alpha=0.3, linestyle='--')
    ax.set_xlim(0, max_time)
    
    # ========================================================================
    # [6] Evicted Keys (Cumulative)
    # ========================================================================
    ax = axes[2, 1]
    add_phase_background(ax, phase_ranges, max_time)
    ax.plot(t, data['evicted_keys'], color='#C62828', linewidth=0.8)
    ax.set_title('Evicted Keys (Cumulative)', fontsize=11)
    ax.set_ylabel('count')
    ax.grid(True, alpha=0.3, linestyle='--')
    ax.set_xlim(0, max_time)
    
    # ========================================================================
    # Common X-axis label
    # ========================================================================
    for ax in axes[2]:
        ax.set_xlabel('Time (seconds)')
    
    # Adjust layout and save
    plt.tight_layout(rect=[0, 0.03, 1, 0.95])
    
    # Create output directory if needed
    output_dir = os.path.dirname(output_path)
    if output_dir and not os.path.exists(output_dir):
        os.makedirs(output_dir, exist_ok=True)
    
    try:
        plt.savefig(output_path, dpi=dpi, bbox_inches='tight', 
                    facecolor='white', edgecolor='none')
        print(f"Report saved: {output_path}")
        print(f"  Image size: {16*dpi}x{12*dpi} pixels at {dpi} DPI")
    except Exception as e:
        print(f"ERROR: Failed to save report: {e}")
        return False
    finally:
        plt.close()
    
    # Show interactive window if requested (requires GUI backend)
    if show:
        # Re-enable interactive backend
        matplotlib.use('TkAgg')
        import matplotlib.pyplot as plt_show
        
        # Recreate the plot with interactive backend
        # (simplified version - just show the saved image)
        print("Note: --show flag requires GUI environment")
    
    return True


# ============================================================================
# Main Entry Point
# ============================================================================

def main():
    """Main entry point for the visualizer."""
    parser = argparse.ArgumentParser(
        description='NUMA Bandwidth Benchmark Visualizer',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    python3 visualize_bw_benchmark.py --input metrics.csv --output report.png
    python3 visualize_bw_benchmark.py --input metrics.csv --output report.png --dpi 200 --show
        """
    )
    parser.add_argument('--input', '-i', required=True, 
                        help='Path to metrics.csv input file')
    parser.add_argument('--output', '-o', required=True, 
                        help='Output PNG file path')
    parser.add_argument('--dpi', type=int, default=150, 
                        help='DPI for output image (default: 150)')
    parser.add_argument('--show', action='store_true', 
                        help='Show plot window (requires GUI)')
    
    args = parser.parse_args()
    
    # Validate input file
    if not os.path.exists(args.input):
        print(f"ERROR: Input file not found: {args.input}")
        sys.exit(1)
    
    # Parse CSV
    print(f"Parsing: {args.input}")
    data, markers, first_ts = parse_csv(args.input)
    
    if not data['time']:
        print("ERROR: No data found in CSV")
        sys.exit(1)
    
    print(f"  Data points: {len(data['time'])}")
    print(f"  Time range: 0 - {max(data['time'])} seconds")
    print(f"  Phases detected: {list(get_phase_ranges(data).keys())}")
    
    # Generate report
    print(f"\nGenerating report: {args.output}")
    success = plot_report(data, markers, first_ts, args.output, args.dpi, args.show)
    
    if success:
        print("\nDone!")
        sys.exit(0)
    else:
        print("\nFailed to generate report")
        sys.exit(1)


if __name__ == '__main__':
    main()
