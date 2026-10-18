# Contributing

Nexora OS is at the foundation stage. Contributions should be small, reviewable,
and supported by evidence. Do not claim a feature, build, test, or hardware
configuration works unless it has been exercised and the result is recorded.

## Before contributing

- Read the [architecture overview](docs/architecture/overview.md) and relevant
  [architecture decisions](docs/decisions/).
- Check the issue or discussion for existing work and agree on broad changes
  before investing in them.
- Preserve existing work. Keep changes scoped to one concern and include
  relevant documentation and tests.
- Do not add third-party product source or binaries without verifying the
  repository, provenance, license, dependencies, and distribution terms.

## Safety and security

- Never run destructive disk or installation commands against the host.
- Build and test images in disposable virtual machines.
- Do not execute user-controlled commands as root from desktop components.
- Do not commit credentials, signing keys, private machine data, or generated
  release artifacts.
- Report suspected vulnerabilities privately as described in [SECURITY.md](SECURITY.md).

## Validation

Run the narrowest relevant test on the documented target environment. For image,
boot, compositor, or hardware changes, record the host and VM configuration,
commands, and observed result. Tests must check behavior, not only file presence
or process startup. The [testing strategy](docs/testing/strategy.md) defines the
current validation gates.

There are no source-code test or build commands yet because no executable
components have been implemented.

## Licensing

Nexora-owned work is licensed under the [MIT License](LICENSE). This does not
change the licenses or obligations of third-party code, assets, or dependencies.
Before adding them, verify and record their provenance, license, and
distribution requirements. Do not contribute material unless you have the
rights needed to offer it under the applicable project terms.
