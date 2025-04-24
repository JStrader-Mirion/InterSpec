# Build Deployable Image:
#  docker build --tag 'interspec_alpine' -f alpine_web_container.dockerfile .

# Run Prebuilt Image:
#   docker run --rm -v path/to/your/datadir:/data -p 8078:8078 interspec_alpine

# Run From Dockerfile Directly:
#   docker run --rm -v path/to/your/datadir:/data -p 8078:8078 -f alpine_web_container.dockerfile .

#  Optional: reuse existing build and/or source directories: Ensure ./build or ./src directory are populated and uncomment the COPY line

# If build fails, add the --no-cache option to the docker build command to force a fresh build
#  docker build --no-cache --tag 'interspec_alpine' -f alpine_web_container.dockerfile .

FROM alpine:3 AS build
ARG REPO=https://github.com/sandialabs/InterSpec.git
ARG BRANCH="master"
WORKDIR /work

# Optional uncomment and populate directories
# COPY src ./src
# COPY build ./build

# RUN statements are broken up to allow loading cached images for debugging
RUN  apk add --no-cache \
        alpine-sdk \
        cmake \
        patch \
        linux-headers \
        suitesparse-dev patch \
        curl \
        uglify-js \
        uglifycss \
        git && \
    if [ ! -d ./src ]; then \
        git clone --recursive --branch $BRANCH --depth=1 $REPO ./src; \
    fi
RUN  cmake \
        -B ./build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_FOR_WEB_DEPLOYMENT=ON \
        -DUSE_REL_ACT_TOOL=ON \
        -DBUILD_AS_LOCAL_SERVER=OFF \
        -DInterSpec_FETCH_DEPENDENCIES=ON \
        -DBoost_INCLUDE_DIR=./build/_deps/boost-src/libs \
        -DUSE_SEARCH_MODE_3D_CHART=ON \
        -DUSE_QR_CODES=ON \
        -DUSE_DETECTION_LIMIT_TOOL=ON \
        ./src
RUN  mkdir -p /InterSpec && \
     cmake --build build -j4
RUN  cmake --install ./build --prefix ./InterSpec && \
        rm -rf ./InterSpec/lib/cmake

#Web Server
FROM alpine:3
LABEL app="InterSpec"
COPY --from=build /work/InterSpec /interspec/
WORKDIR /interspec
EXPOSE 8078
RUN apk --no-cache add \
        openblas \
        libstdc++ \
        libgcc && \
        chmod -R a+r * && \
        chmod a+x bin/InterSpec &&  \
        chmod 777 /interspec
SHELL ["/bin/sh", "-c"]
ENTRYPOINT ["./bin/InterSpec", "--config ./share/interspec/data/config/wt_config_web.xml", "--userdatadir=/data", "--http-port=8078", "--http-address=0.0.0.0", "--docroot", "./share/interspec"]


# Then numeric group/user value of 280 was chosen randomly; it doesnt conflict with existing groups/users on dev or public server, and is below 1000 (e.g., a system user without a home directory or default shell)
#RUN groupadd --gid 280 interspec && useradd --uid 280 --gid interspec interspec
#RUN addgroup -S interspec && adduser --disabled-password --no-create-home -S interspec -G interspec
#USER interspec
# Or just use user guest
#USER guest
# You could keep the access log by chenging the entrypoint to: "--accesslog=/mnt/interspec_data/wt_access_log.txt"
# You could also edit the <log-file></log-file> element of data/config/wt_config_web.xml to save the stdout/stderr of InterSpec to a log file at /mnt/interspec_data/interspec_log.txt.