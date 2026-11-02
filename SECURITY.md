# Security

rbscan reads HDF5 files and JSON schemas. Treat scans from untrusted sources
like any other untrusted input: the bundled HDF5 library parses the file
structure, and a malformed file can make it fail or worse.

Please report vulnerabilities privately to the maintainers at RayBotic, not in
a public issue. Use GitHub's "Report a vulnerability" (private security
advisory) on the mirror:
<https://github.com/RayBotic-NDT/rbscan/security/advisories/new>.
We will confirm receipt and work on a fix and release.

Fixes are released for the latest version only.
