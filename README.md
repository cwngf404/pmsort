# Pmsort
pmsort (partition merge sort) is an adaptive, stable sorting algorithm by using the multi-way distribution partitioning to achieve higher performance on modern CPUs. Codes are available for free under MIT license. 

```
Best | Average    | Worst      | Deterministics | Stable
O(n) | O(n log n) | O(n log n) | Yes            | Yes
```
```
Memory: O(n)
```

# Usage

### Require: C++17+

### Using:
```cpp
pmsort(first, last, comparator);
pmsort(container, comparator);
```

`pmsort` is a drop-in replacement for [`std::stable_sort()`](https://en.cppreference.com/cpp/algorithm/stable_sort). Just include the header and call ```pmsort()``` to start using partition merge sort.

# Visualization
This visualization below shows how `pmsort` sorting 256 random elements 
![Visualization](imgs/pms256.gif)

# Benchmark
 OS: `Ubuntu 24.04.5 LTS x86_64`<br>
 Compiler: `g++` <br>
 Compiler flags: `-O3 -march=native -std=c++20`

 
 ![benchmark](imgs/bench.png)

 **Results(10 runs, N=10'000'000):**
 
| Case | pmsort | std::sort | std::stable_sort |
| :--- | :---: | :---: | :---: |
| Random | **922ms** | 1465ms | 1832ms |
| Duplicate | **831ms** | 1019ms | 1370ms |
| Nearly Sorted | **167ms** | 223ms | 262ms |
| Pipe Organ | **81ms** | 1764ms | 313ms |
| Reversed | **12ms** | 216ms | 314ms |
| Zipf | **424ms** | 827ms | 996ms |

*This does not mean `pmsort` always faster than [`std::sort`](https://en.cppreference.com/cpp/algorithm/sort), there are many implementations across different compilers*

