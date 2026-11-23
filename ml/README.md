# EpiAlert Edge-ML & Hyperdimensional Computing (HDC) Pipeline

> **Notice Regarding Source Code Availability:**  
> The Python Multi-Centroid Hyperdimensional Computing (HDC) training and continuous learning codebase, adapted from Pale et al (2022), is still being validated, and will be published shortly.

---

### Algorithmic Architecture Highlights
- **Binary Hyperdimensional Computing**: Encodes 3-axis accelerometer jerk, blood volume pulse (BVP), and skin temperature into 10,000-dimensional binary hypervectors ($\mathcal{H} \in \{0, 1\}^D$).
- **Multi-Centroid Clustering**: Generates localized sub-centroids to address severe real-world class imbalance (1:1338 ratio).
- **On-Device Inference**: Hardware inference reduces to simple XOR and bit-count (Hamming distance) operations, executing in just **7.49 ms** on the Feather M0 microcontroller with zero cloud dependencies.
