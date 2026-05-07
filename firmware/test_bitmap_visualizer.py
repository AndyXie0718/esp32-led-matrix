#!/usr/bin/env python3
"""
Test program to visualize 8x8 LED matrix bitmap patterns.
Reads binary strings from stdin and displays them as images.
"""

import sys
import matplotlib.pyplot as plt
import numpy as np


def normalize_bitmap(bitmap: str, width: int, height: int) -> str:
    """
    Normalize a bitmap string to the expected dimensions.
    
    Args:
        bitmap: Input bitmap string (may contain any characters)
        width: Expected width
        height: Expected height
    
    Returns:
        Normalized bitmap string with only 0s and 1s
    """
    # Extract only 0s and 1s
    bits = ''.join(c for c in bitmap if c in '01')
    
    total_pixels = width * height
    
    if len(bits) >= total_pixels:
        # Truncate to expected size
        return bits[:total_pixels]
    elif len(bits) == 0:
        # Return empty string
        return ''
    else:
        # Pad with zeros
        return (bits + '0' * total_pixels)[:total_pixels]


def bitmap_to_grid(bitmap: str, width: int, height: int) -> np.ndarray:
    """
    Convert a bitmap string to a 2D grid (numpy array).
    
    Args:
        bitmap: Binary string (0s and 1s)
        width: Expected width
        height: Expected height
    
    Returns:
        2D numpy array of shape (height, width) with 0s and 1s
    """
    normalized = normalize_bitmap(bitmap, width, height)
    
    if len(normalized) != width * height:
        raise ValueError(f"Invalid bitmap length: expected {width*height}, got {len(normalized)}")
    
    grid = np.zeros((height, width), dtype=int)
    for i, ch in enumerate(normalized):
        row = i // width
        col = i % width
        grid[row, col] = int(ch)
    
    return grid


def visualize_grid(grid: np.ndarray, title: str = "LED Matrix Pattern", 
                   cmap: str = 'YlOrRd', show: bool = True) -> None:
    """
    Visualize a 2D grid as an image.
    
    Args:
        grid: 2D numpy array
        title: Plot title
        cmap: Matplotlib colormap name
        show: Whether to display the plot
    """
    fig, ax = plt.subplots(figsize=(6, 6))
    
    # Display the grid as an image
    im = ax.imshow(grid, cmap=cmap, interpolation='nearest')
    
    # Add colorbar
    plt.colorbar(im, ax=ax, label='LED State')
    
    # Set title and labels
    ax.set_title(title, fontsize=14, fontweight='bold')
    ax.set_xlabel('Column')
    ax.set_ylabel('Row')
    
    # Add grid lines
    ax.set_xticks(np.arange(-0.5, grid.shape[1], 1), minor=True)
    ax.set_yticks(np.arange(-0.5, grid.shape[0], 1), minor=True)
    ax.grid(which='minor', color='black', linestyle='-', linewidth=2)
    
    # Add cell values
    for i in range(grid.shape[0]):
        for j in range(grid.shape[1]):
            ax.text(j, i, str(grid[i, j]), ha='center', va='center',
                   color='white' if grid[i, j] == 1 else 'black',
                   fontsize=16, fontweight='bold')
    
    plt.tight_layout()
    
    if show:
        plt.show()
    
    return fig, ax


def get_resolution() -> tuple:
    """
    Get resolution from user input.
    
    Returns:
        Tuple of (width, height)
    """
    while True:
        print("\n" + "=" * 60)
        print("Select Resolution")
        print("=" * 60)
        print("\nAvailable presets:")
        print("  1. 8x8   (64 pixels) - Default")
        print("  2. 16x16 (256 pixels)")
        print("  3. 32x32 (1024 pixels)")
        print("  4. Custom...")
        print()
        
        choice = input("Enter choice (1-4): ").strip()
        
        if choice == '1':
            return 8, 8
        elif choice == '2':
            return 16, 16
        elif choice == '3':
            return 32, 32
        elif choice == '4':
            try:
                width = int(input("Enter width (e.g., 8, 16, 32): ").strip())
                height = int(input("Enter height (e.g., 8, 16, 32): ").strip())
                
                if width <= 0 or height <= 0:
                    print("Error: Width and height must be positive integers.")
                    continue
                
                if width > 64 or height > 64:
                    print("Warning: Large resolutions may require more memory.")
                
                return width, height
            except ValueError:
                print("Error: Please enter valid integers.")
                continue
        else:
            print("Invalid choice. Please enter 1-4.")


def print_grid(grid: np.ndarray) -> None:
    """Print the grid as text."""
    print("\nGrid visualization:")
    print("┌" + "───" * grid.shape[1] + "┐")
    for row in grid:
        line = "│ " + " ".join(f"{int(x)} " for x in row) + " │"
        print(line)
    print("└" + "───" * grid.shape[1] + "┘\n")


def main():
    """Main function to read bitmap from stdin and visualize it."""
    # Get resolution from user
    width, height = get_resolution()
    
    print("\n" + "=" * 60)
    print(f"8x{width} LED Matrix Bitmap Visualizer")
    print(f"Resolution: {width}x{height} ({width*height} pixels)")
    print("=" * 60)
    print("\nInstructions:")
    print(f"  - Enter a {width}x{height} binary string ({width*height} characters of 0s and 1s)")
    print("  - Or paste a bitmap pattern")
    print("  - Press Enter twice to visualize")
    print("  - Type 'quit' or 'exit' to exit")
    print()
    
    while True:
        try:
            # Read input
            print(f"Enter bitmap ({width*height} bits): ", end='', flush=True)
            bitmap_input = sys.stdin.readline().strip()
            
            if not bitmap_input:
                continue
            
            # Check for exit commands
            if bitmap_input.lower() in ('quit', 'exit', 'q'):
                print("Exiting...")
                break
            
            # Validate input length
            expected_length = width * height
            if len(bitmap_input) != expected_length:
                print(f"Warning: Input length is {len(bitmap_input)}, expected {expected_length}. Normalizing...")
            
            # Convert to grid
            try:
                grid = bitmap_to_grid(bitmap_input, width, height)
            except ValueError as e:
                print(f"Error: {e}")
                continue
            
            # Print text representation
            print_grid(grid)
            
            # Visualize
            print("Generating visualization...")
            fig, ax = visualize_grid(
                grid,
                title=f"{width}x{height} LED Matrix (Input: {bitmap_input[:20]}...)",
                cmap='YlOrRd',
                show=True
            )
            
            print("Visualization complete!")
            print()
            
        except KeyboardInterrupt:
            print("\nInterrupted by user.")
            break
        except EOFError:
            print("\nEnd of input.")
            break
        except Exception as e:
            print(f"Error: {e}")
            continue
    
    print("\nThank you for using the LED Matrix Visualizer!")
    print("=" * 60)


if __name__ == "__main__":
    main()
