%define vendor_name lustre
%define _version %(if test -s "%_sourcedir/_version"; then cat "%_sourcedir/_version"; else echo "UNKNOWN"; fi)
%define flavor default

%define intranamespace_name %{vendor_name}-%{flavor}

%define source_name %{vendor_namespace}-%{vendor_name}-%{_version}
%define branch trunk

%define kernel_version %(rpm -q --qf '%{VERSION}' kernel-source)
%define kernel_release %(rpm -q --qf '%{RELEASE}' kernel-source)
%define kernel_release_major %(rpm -q --qf "%{RELEASE}" kernel-source |  awk -F . '{print $1}')

%define cray_kernel_version %(make -s -C /usr/src/linux-obj/%{_target_cpu}/%{flavor} kernelrelease)
# Override the _mandir so man pages don't end up in /man
%define pc_files cray-lustre-api-devel.pc cray-lustre-cfsutil-devel.pc cray-lustre-ptlctl-devel.pc
%define _mandir /usr/share/man

BuildRequires: kernel-source
BuildRequires: kernel-syms
BuildRequires: pkgconfig
BuildRequires: -post-build-checks
BuildRequires: module-init-tools
BuildRequires: libtool
%if "%{?sle_version}" == "120000"
# Only SLES 12 SP0 builds require this. Was needed for EDR IB support in eLogin
# for 6.0UP02. Future versions will use in-kernel drivers.
BuildRequires: ofed-devel
%endif
Group: System/Filesystems
License: GPL
Name: %{namespace}-%{intranamespace_name}
Release: %{release}
Summary: Lustre File System for CLFS SLES-based Nodes
Version: %{_version}_%{kernel_version}_%{kernel_release}
Source0: %{source_name}.tar.bz2
Source1: %{vendor_namespace}-%{vendor_name}-switch-%{_version}.tar.bz2
URL: %url
BuildRoot: %{_tmppath}/%{name}-%{version}-root

# Override _prefix to avoid installing into Cray locations under /opt/cray/
%define _prefix    /
%define _includedir /usr/include

%description
Kernel modules and userspace tools needed for a Lustre client on CLFS SLES-based
service nodes.

%prep
# using source_name here results in too deep of a macro stack, so use
# definition of source_name directly
%incremental_setup -q -n %{source_name} -a 1

%build
echo "LUSTRE_VERSION = %{_tag}" > LUSTRE-VERSION-FILE
# LUSTRE_VERS used in ko versioning.
%define version_path %(basename %url)
%define date %(date +%%F-%%R)
%define lustre_version %{branch}-%{release}-%{build_user}-%{version_path}-%{date}
export LUSTRE_VERS=%{lustre_version}
export SVN_CODE_REV=%{_version}-${LUSTRE_VERS}

if [ "%reconfigure" == "1" -o ! -x %_builddir/%{source_name}/configure ];then
        chmod +x autogen.sh
        ./autogen.sh
fi

if [ -d /usr/src/kernel-modules-ofed/%{_target_cpu}/%{flavor} ]; then
    _with_o2ib="--with-o2ib=/usr/src/kernel-modules-ofed/%{_target_cpu}/%{flavor}"
    _with_symvers="--with-symvers=/usr/src/kernel-modules-ofed/%{_target_cpu}/%{flavor}/Modules.symvers"
fi

CFLAGS="%{optflags} -Werror"

if [ "%reconfigure" == "1" -o ! -f %_builddir/%{source_name}/Makefile ];then
        %configure --disable-checksum \
           --disable-server \
           --with-linux-obj=/usr/src/linux-obj/%{_target_cpu}/%{flavor} \
           ${_with_o2ib} \
           ${_with_symvers} \
           --with-obd-buffer-size=16384
fi
%{__make} %_smp_mflags

%install
# don't use %makeinstall for Rhine RPMS - it needlessly puts things into 
# /opt/cray/...

make DESTDIR=${RPM_BUILD_ROOT} install 

pushd %{buildroot}

if [ -e etc ]
then
    for f in lustre lhbadm ldev haconfig; do
        %{__rm} -f etc/init.d/${f}
    done
    %{__rm} -rf etc/ha.d etc/sysconfig etc/ldev.conf
fi

popd

# set l_getidentity to the default location
%{__mkdir_p} %{buildroot}/usr/sbin
%{__ln_s} -f /sbin/l_getidentity %{buildroot}/usr/sbin/l_getidentity

for file in libcfsutil.a libiam.a liblustre.a liblustre.so liblustreapi.a liblustreapi.so libptlctl.a
do
    found=`find %{buildroot} -name $file`
    [ -n "${found}" ] && install -D -m 0644 ${found} %{buildroot}/usr/lib64/${file}
done

for f in %{pc_files}
do
    eval "sed -i 's,^prefix=.*$,prefix=/usr,' %{_sourcedir}/${f}"
    install -D -m 0644  %{_sourcedir}/${f} %{buildroot}/%{_pkgconfigdir}/${f}
    %{__rm} -f %{_sourcedir}/${f}
done

%{__sed} -e 's/@VERSION@/%{version}-%{release}/g' version.in > .version

# Install module directories and files
%{__install} -D -m 0644 .version %{buildroot}/%{_name_modulefiles_prefix}/.version
%{__install} -D -m 0644 module %{buildroot}/%{_release_modulefile}

%post
%{__ln_s} %{_sbindir}/ko2iblnd-probe /usr/sbin

DEPMOD_OPTS=""
if [ -f /boot/System.map-%{cray_kernel_version} ]; then
    DEPMOD_OPTS="-F /boot/System.map-%{cray_kernel_version}"
fi

depmod -a ${DEPMOD_OPTS} %{cray_kernel_version}

%preun
%{__rm} -f /usr/sbin/ko2iblnd-probe

%files
%defattr(-,root,root)
%{_prefix}
%exclude %{_sysconfdir}/lustre/perm.conf

%clean
%clean_build_root
