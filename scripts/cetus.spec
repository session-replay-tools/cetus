%define _prefix /usr/local/cetus
%define _bindir %{_prefix}/bin
%define _libdir %{_prefix}/lib
%define _conf %{_prefix}/conf
%define _libexec %{_prefix}/libexec
%define _logs  %{_prefix}/logs
%define _simple_parser ON
#
# Simple RPM spec file for Cetus
# written by lede
#
Summary: MySQL Proxy
Name: cetus
Version: 1.0
Release: 1%{?dist}
License: GPL
Group: Applications/Networking
Source: %{name}-%{version}.tar.gz
Prefix: %{_prefix}
Buildroot: %{_tmppath}/%{name}-%{version}-%{release}-root
Packager: lede
Requires: glib2-devel libevent-devel mysql-devel
BuildRequires: cmake gcc flex mysql-devel glib2-devel libevent-devel openssl-devel

%description
Cetus is a simple program that sits between your client and MySQL
server(s) that can monitor, analyze or transform their communication. Its
flexibility allows for unlimited uses; common ones include: load balancing;
failover; query analysis; query filtering and modification; and many more.

%prep
%setup -q -n %{name}-%{version}

%build
if [ ! -d "bld" ]; then
    mkdir bld
fi

cd bld
rm -rf CMakeCache.txt

cmake ../ -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%{_prefix} -DSIMPLE_PARSER=%{_simple_parser}

%install
cd bld
%{__make} DESTDIR=%{buildroot} install

%clean
%{__rm} -rfv %{buildroot}

%post

%postun

%files
%defattr(-,root,root)
%{_bindir}/*
%{_libdir}/*
%{_conf}/*
%{_libexec}/*
%{_logs}/*

%doc

%changelog

