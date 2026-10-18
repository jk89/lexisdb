# Releasing Lexis

How the Lexis Docker images are built and published. The images are x86_64
only.

## Docker Hub login

    docker login

Docker reuses saved credentials, or offers a browser login. The server image is
built with buildah but pushed through Docker, so no buildah login is needed.

## Server image (0x6a6f6e6e79/lexisdb)

1. Fetch the submodules, which point at the Lexis cqlsh and tools-java:

        git submodule sync && git submodule update --init --recursive

2. Build the Lexis toolchain image: the ScyllaDB build toolchain with
   lexis-python-driver in place of scylla-driver, so that the packaged python3
   and cqlsh, and the image built from them, ship the Lexis driver:

        docker build -t lexis-toolchain -f tools/toolchain/Dockerfile.lexis tools/toolchain

3. Configure a release build:

        ./tools/toolchain/dbuild --image lexis-toolchain -- ./configure.py --mode=release

4. Build the release packages (server, python3, cqlsh and tools). This takes a
   few hours; if the compiler runs out of memory, add `-j8`:

        ./tools/toolchain/dbuild --image lexis-toolchain -- ninja dist-release

   The Debian packages land in `build/dist/release/debian/`,
   `tools/cqlsh/build/debian/` and `tools/python3/build/debian/`.

5. Build the image on the host (not inside dbuild), with buildah
   (`sudo apt install -y buildah`):

        ./dist/docker/debian/build_docker.sh --mode release

   It writes an OCI archive and prints its name, for example
   `oci-archive:build/release/dist/docker/scylla-6.2.3-0.20261018.b578d92d8cb2`.
   The file and the packages keep upstream's product name, `scylla`; the
   published name is set in the next step.

6. Load the archive into Docker under the published name. `buildah push` only
   reads images from buildah's own store, so pull the archive in first:

        buildah pull oci-archive:build/release/dist/docker/scylla-<version>
        buildah tag $(buildah images -q | head -1) 0x6a6f6e6e79/lexisdb:latest
        buildah push 0x6a6f6e6e79/lexisdb:latest docker-daemon:0x6a6f6e6e79/lexisdb:latest

7. Smoke-test it; `token(key)` should print a hex blob:

        docker run --name lexisdb-test -d 0x6a6f6e6e79/lexisdb:latest
        sleep 60; docker exec -it lexisdb-test cqlsh -e "SELECT release_version FROM system.local; SELECT token(key) FROM system.local;"

   If cqlsh cannot connect, the node is still starting or has failed; its log
   says which:

        docker logs lexisdb-test 2>&1 | tail -40

8. Push to Docker Hub as `latest` and as the Lexis version (`1.0.0` for the
   first release). Keep the test container if you are building the cqlsh image
   next, since its test uses it; otherwise remove it:

        docker tag 0x6a6f6e6e79/lexisdb:latest 0x6a6f6e6e79/lexisdb:<lexis-version>
        docker push 0x6a6f6e6e79/lexisdb:latest
        docker push 0x6a6f6e6e79/lexisdb:<lexis-version>
        docker rm -f lexisdb-test

## cqlsh image (0x6a6f6e6e79/lexis-cqlsh)

Built from the lexis-cqlsh repository's Dockerfile, which installs cqlsh and
lexis-python-driver. Test it against a running lexisdb test container (step 7)
before pushing; `token(key)` should print `0x6c6f63616c0000`:

    cd lexis-cqlsh
    docker build -t 0x6a6f6e6e79/lexis-cqlsh:latest -t 0x6a6f6e6e79/lexis-cqlsh:<lexis-version> .
    docker run --rm -it 0x6a6f6e6e79/lexis-cqlsh:latest $(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' lexisdb-test) -e "SELECT token(key) FROM system.local;"
    docker push 0x6a6f6e6e79/lexis-cqlsh:latest
    docker push 0x6a6f6e6e79/lexis-cqlsh:<lexis-version>

## Gotchas

- The repositories contain symlinks (for example `tools/cqlsh/setup.py` and the
  files under `dist/debian`). Check them out with `core.symlinks` enabled, or
  they become plain text files and the cqlsh and Debian packaging fail. To
  repair a checkout:

        git config core.symlinks true
        git ls-files -s | awk '$1=="120000"{print $4}' | xargs rm -rf
        git checkout -- $(git ls-files -s | awk '$1=="120000"{print $4}')

  Repeat inside `tools/cqlsh` and `tools/java`.
- File permissions must match git's. Files checked out with a stray executable
  bit (hidden when `core.fileMode` is false) break the Debian packaging:
  debhelper runs an executable `debian/*.install` file as a script. To reset
  them, in the repository and in each submodule:

        git config core.fileMode true
        git ls-files -s | awk '$1=="100644"{print $4}' | xargs -d '\n' chmod a-x
        git ls-files -s | awk '$1=="100755"{print $4}' | xargs -d '\n' chmod a+x

  After repairing symlinks or permissions, delete the packaging outputs built
  from the bad checkout so they are rebuilt (compiled objects can stay):

        rm -rf tools/java/build tools/cqlsh/build tools/python3/build build/release/dist build/dist

- The repository has a Git LFS pre-push hook but tracks no LFS files. Without
  `git-lfs` installed, push with `git push --no-verify`.
- When the build fails, the error usually scrolls past. Save the output and
  look for the first `FAILED:` line:

        ./tools/toolchain/dbuild --image lexis-toolchain -- ninja dist-release 2>&1 | tee build-release.log
        grep -n -A30 'FAILED' build-release.log | head -80
- A node that dies at startup with `Could not setup Async I/O ... aio-max-nr`
  has run out of kernel AIO slots, shared by every process on the host
  (`cat /proc/sys/fs/aio-nr /proc/sys/fs/aio-max-nr`). Stop other Scylla or
  Lexis processes, or raise the limit as ScyllaDB recommends:

        sudo sysctl -w fs.aio-max-nr=1048576
- The container's IP is not `.NetworkSettings.IPAddress` in current Docker;
  read it per network:

        docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' lexisdb-test
