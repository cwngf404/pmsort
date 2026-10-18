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
pms(first, last, comparator);
pms(container, comparator);
```

`pmsort` is a drop-in replacement for [`std::stable_sort()`](https://en.cppreference.com/cpp/algorithm/stable_sort). Just include the header and call ```pms()``` to start using partition merge sort.

# Visualization
This visualization below shows how `pmsort` sorting 256 random elements 
![Visualization](imgs/pms256.gif)

# Benchmark
 OS: `Ubuntu 24.04.5 LTS x86_64`<br>
 Compiler flags: `-O3 -march=native -std=c++20`

 
 ![benchmark](imgs/bench.png)
