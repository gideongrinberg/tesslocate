FROM debian:bookworm-slim

RUN apt-get update && apt-get install --yes build-essential procps curl file git && \
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

ENV PATH="/home/linuxbrew/.linuxbrew/bin:/home/linuxbrew/.linuxbrew/sbin:${PATH}"
RUN brew install libomp nlohmann-json curl cmake libarchive abseil

WORKDIR /app
RUN mkdir -p s2/ && curl -L https://github.com/google/s2geometry/archive/refs/tags/v0.11.1.zip -o - | bsdtar xvf - -C s2 --strip-components=1 && \
    cd s2 && mkdir build && cd build && cmake -DWITH_GFLAGS=OFF -DWITH_GTEST=OFF -DBUILD_TESTS=OFF .. && make -j$(nproc) && make install

COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --target tesslocate

CMD ["./build/tesslocate"]