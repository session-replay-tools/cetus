#!/bin/bash

basedir=$(pwd)

set -e

while getopts "v:r:s:h" arg
do
        case $arg in
             v)
                echo "version_no: $OPTARG"
                version_no=$OPTARG
		;;
             r)
                echo "release_no: $OPTARG"
		release_no=$OPTARG
                ;;
             s)
                echo "simple_parser: $OPTARG"
                simple_parser=$OPTARG
                ;;
	     h)
                echo "-v release_no -r release_no -s simple_parser"
		exit 0
		;;
             ?)
            	echo "unkown argument"
        	exit 1
        	;;
        esac
done

if [[ ! "${version_no}" ]] || [[ ! "${release_no}" ]];then
	echo "error argument, plese exec with '-v version_no -r release_no'"
	exit 1
fi

if [[ ! "${simple_parser}" ]];then
    simple_parser="1"
    echo "simple_parser: ${simple_parser}"
fi

rm -rf ~/rpmbuild/RPMS/x86_64/cetus-${version_no}-${release_no}.el6.x86_64.rpm
rm -rf ~/rpmbuild/RPMS/x86_64/cetus-debuginfo-${version_no}-${release_no}.el6.x86_64.rpm
rm -rf ~/rpmbuild/SRPMS/cetus-${version_no}-${release_no}.el6.src.rpm

rm -rf ~/rpmbuild/SOURCES/cetus-${version_no}
rm -rf ~/rpmbuild/SPECS/cetus-${version_no}.spec
mkdir ~/rpmbuild/SOURCES/cetus-${version_no}
cp -rf ../* ~/rpmbuild/SOURCES/cetus-${version_no}
cp -rf ./cetus.spec ~/rpmbuild/SPECS/cetus-${version_no}.spec


sed -i "s/Version:.*/Version: ${version_no}/"  ~/rpmbuild/SPECS/cetus-${version_no}.spec
sed -i "s/Release:.*/Release: ${release_no}%{?dist}/" ~/rpmbuild/SPECS/cetus-${version_no}.spec
if [ ${simple_parser} -eq 0 ]; then
    sed -i "s/%define _simple_parser .*/%define _simple_parser off/" ~/rpmbuild/SPECS/cetus-${version_no}.spec
fi

cd ~/rpmbuild/SOURCES/
tar -zcf cetus-${version_no}.tar.gz cetus-${version_no}
cd ~
rpmbuild -ba ~/rpmbuild/SPECS/cetus-${version_no}.spec

cd $basedir
mv ~/rpmbuild/RPMS/x86_64/cetus-${version_no}-${release_no}.el6.x86_64.rpm ./
mv ~/rpmbuild/RPMS/x86_64/cetus-debuginfo-${version_no}-${release_no}.el6.x86_64.rpm ./
mv ~/rpmbuild/SRPMS/cetus-${version_no}-${release_no}.el6.src.rpm ./

rm -rf ~/rpmbuild/SOURCES/cetus-${version_no}
rm -rf ~/rpmbuild/SOURCES/cetus-${version_no}.tar.gz
rm -rf ~/rpmbuild/BUILD/cetus-${version_no}
rm -rf ~/rpmbuild/SPECS/cetus-${version_no}.spec
