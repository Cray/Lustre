%define intranamespace_name lnet
%ifarch k1om
%define flavor cray_ari_m
%else
%if %{with athena}
%define flavor cray_ari_athena_s_cos
%else
%define flavor cray_ari_s
%endif
%endif
%define version 0.1
%define lustre_version master
%define branch trunk

%define pkgbase %{namespace}-%{intranamespace_name}
%define pkgname %{pkgbase}-%{lustre_version}-devel
%define pkg_config_name %{pkgbase}.pc

%define srcbase %{namespace}-lustre
%define pkgsrcbase %{pkgbase}-devel-%{branch}

Group: Development/Libraries/C and C++
License: GPL
Name: %{pkgname}
Release: %release
Requires: %{switch_requires}
BuildRequires: update-alternatives
BuildRequires: kernel-source
BuildRequires: kernel-syms
BuildRequires: module-init-tools
BuildRequires: pkgconfig
BuildRequires: libtool
Summary: The lnet development package
Vendor: Cray Inc.
Version: %{branch}
Source0: %{srcbase}.tar.gz
Source1: %{pkgsrcbase}.tar.gz
URL: %url
BuildRoot: %{_tmppath}/%{name}-%{version}-root

%description
Header files for lnet.

%prep
# using source_name here results in too deep of a macro stack, so use
# definition of source_name directly
%incremental_setup -q -n %{srcbase} -a 1

%build
if [ "%reconfigure" == "1" -o ! -x %_builddir/%{source_name}/configure ];then
        chmod +x autogen.sh
        ./autogen.sh
fi
if [ "%reconfigure" == "1" -o ! -f %_builddir/%{source_name}/Makefile ];then
        %configure --disable-checksum \
           --disable-server \
           --with-linux-obj=/usr/src/linux-obj/%{_target_cpu}/%{flavor} \
           --with-o2ib=no \
           --disable-manpages \
           --with-obd-buffer-size=16384
fi
pushd %{pkgsrcbase}
%CRAYconfigure -- --with-module=%{_release_modulefile}

%{__make} %_smp_mflags

%install
install -D -m 0644 config.h %{buildroot}/%{_includedir}/config.h
for header in `find lnet/include lustre/include libcfs/include -name \*.h`
do
	install -D -m 0644 ${header} %{buildroot}/%{_includedir}/${header}
done
install -D -m 0644  %{pkgsrcbase}/%{pkgbase}-devel.pc %{buildroot}/%{_pkgconfigdir}/%{pkg_config_name}
install -D -m 0644  %{pkgsrcbase}/module %{buildroot}/%{_release_modulefile}
install -D -m 0644  %{pkgsrcbase}/.version %{buildroot}/%{_release_prefix}/etc/.version

%files
%defattr(-,root,root)
%prefixdirs
%if %{without lustre}
%dir %{_namespace_modulefiles_network_prefix}
%endif
%dir %{_modulefiles_prefix}
%dir %{_modulefiles_prefix}/lnet
%dir %{_sysconfdir}
%dir %{_libdir}
%{_includedir}
%{_pkgconfigdir}
%{_release_modulefile}
%{_sysconfdir}

%post
/usr/sbin/update-alternatives --install %{_default_prefix} %{pkgbase} %{_release_prefix} %{release_priority}
%{__ln_s} -f %{_default_sysconfdir}/.version %{_name_modulefiles_prefix}/.version
%link_pkg_config -n %{pkgbase}

%preun
%{__rm} -f %{_name_modulefiles_prefix}/.version
/usr/sbin/update-alternatives --remove %{pkgbase} %{_release_prefix}
%remove_pkg_config -n %{pkgbase}
