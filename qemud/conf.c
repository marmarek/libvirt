/*
 * config.c: VM configuration management
 *
 * Copyright (C) 2006, 2007 Red Hat, Inc.
 * Copyright (C) 2006 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <dirent.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/uri.h>

#include <libvirt/virterror.h>

#include "protocol.h"
#include "internal.h"
#include "conf.h"
#include "driver.h"

static int qemudParseUUID(const char *uuid,
                          unsigned char *rawuuid) {
    const char *cur;
    int i;

    /*
     * do a liberal scan allowing '-' and ' ' anywhere between character
     * pairs as long as there is 32 of them in the end.
     */
    cur = uuid;
    for (i = 0;i < 16;) {
        rawuuid[i] = 0;
        if (*cur == 0)
            goto error;
        if ((*cur == '-') || (*cur == ' ')) {
            cur++;
            continue;
        }
        if ((*cur >= '0') && (*cur <= '9'))
            rawuuid[i] = *cur - '0';
        else if ((*cur >= 'a') && (*cur <= 'f'))
            rawuuid[i] = *cur - 'a' + 10;
        else if ((*cur >= 'A') && (*cur <= 'F'))
            rawuuid[i] = *cur - 'A' + 10;
        else
            goto error;
        rawuuid[i] *= 16;
        cur++;
        if (*cur == 0)
            goto error;
        if ((*cur >= '0') && (*cur <= '9'))
            rawuuid[i] += *cur - '0';
        else if ((*cur >= 'a') && (*cur <= 'f'))
            rawuuid[i] += *cur - 'a' + 10;
        else if ((*cur >= 'A') && (*cur <= 'F'))
            rawuuid[i] += *cur - 'A' + 10;
        else
            goto error;
        i++;
        cur++;
    }

    return 0;

 error:
    return -1;
}


struct qemu_arch_info {
    const char *arch;
    const char **machines;
    const char *binary;
};

/* The list of possible machine types for various architectures,
   as supported by QEMU - taken from 'qemu -M ?' for each arch */
static const char *arch_info_x86_machines[] = {
    "pc", "isapc"
};
static const char *arch_info_mips_machines[] = {
    "mips"
};
static const char *arch_info_sparc_machines[] = {
    "sun4m"
};
static const char *arch_info_ppc_machines[] = {
    "g3bw", "mac99", "prep"
};

/* The archicture tables for supported QEMU archs */
static struct qemu_arch_info archs[] = { 
    {  "i686", arch_info_x86_machines, "qemu" },
    {  "x86_64", arch_info_x86_machines, "qemu-system-x86_64" },
    {  "mips", arch_info_mips_machines, "qemu-system-mips" },
    {  "mipsel", arch_info_mips_machines, "qemu-system-mipsel" },
    {  "sparc", arch_info_sparc_machines, "qemu-system-sparc" },
    {  "ppc", arch_info_ppc_machines, "qemu-system-ppc" },
};

/* Return the default architecture if none is explicitly requested*/
static const char *qemudDefaultArch(void) {
    return archs[0].arch;
}

/* Return the default machine type for a given architecture */
static const char *qemudDefaultMachineForArch(const char *arch) {
    int i;

    for (i = 0 ; i < (int)(sizeof(archs) / sizeof(struct qemu_arch_info)) ; i++) {
        if (!strcmp(archs[i].arch, arch)) {
            return archs[i].machines[0];
        }
    }

    return NULL;
}

/* Return the default binary name for a particular architecture */
static const char *qemudDefaultBinaryForArch(const char *arch) {
    int i;

    for (i = 0 ; i < (int)(sizeof(archs) / sizeof(struct qemu_arch_info)) ; i++) {
        if (!strcmp(archs[i].arch, arch)) {
            return archs[i].binary;
        }
    }

    return NULL;
}

/* Find the fully qualified path to the binary for an architecture */
static char *qemudLocateBinaryForArch(struct qemud_server *server,
                                      int virtType, const char *arch) {
    const char *name;
    char *path;

    if (virtType == QEMUD_VIRT_KVM)
        name = "qemu-kvm";
    else
        name = qemudDefaultBinaryForArch(arch);

    if (!name) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "cannot determin binary for architecture %s", arch);
        return NULL;
    }

    /* XXX lame. should actually use $PATH ... */
    path = malloc(strlen(name) + strlen("/usr/bin/") + 1);
    if (!path) {
        qemudReportError(server, VIR_ERR_NO_MEMORY, "path");
        return NULL;
    }
    strcpy(path, "/usr/bin/");
    strcat(path, name);
    return path;
}

/* Parse the XML definition for a disk */
static struct qemud_vm_disk_def *qemudParseDiskXML(struct qemud_server *server,
                                                   xmlNodePtr node) {
    struct qemud_vm_disk_def *disk = calloc(1, sizeof(struct qemud_vm_disk_def));
    xmlNodePtr cur;
    xmlChar *device = NULL;
    xmlChar *source = NULL;
    xmlChar *target = NULL;
    xmlChar *type = NULL;
    int typ = 0;

    if (!disk) {
        qemudReportError(server, VIR_ERR_NO_MEMORY, "disk");
        return NULL;
    }

    type = xmlGetProp(node, BAD_CAST "type");
    if (type != NULL) {
        if (xmlStrEqual(type, BAD_CAST "file"))
            typ = QEMUD_DISK_FILE;
        else if (xmlStrEqual(type, BAD_CAST "block"))
            typ = QEMUD_DISK_BLOCK;
        else {
            typ = QEMUD_DISK_FILE;
        }
        xmlFree(type);
        type = NULL;
    }

    device = xmlGetProp(node, BAD_CAST "device");
  
    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if ((source == NULL) &&
                (xmlStrEqual(cur->name, BAD_CAST "source"))) {
	
                if (typ == QEMUD_DISK_FILE)
                    source = xmlGetProp(cur, BAD_CAST "file");
                else
                    source = xmlGetProp(cur, BAD_CAST "dev");
            } else if ((target == NULL) &&
                       (xmlStrEqual(cur->name, BAD_CAST "target"))) {
                target = xmlGetProp(cur, BAD_CAST "dev");
            } else if (xmlStrEqual(cur->name, BAD_CAST "readonly")) {
                disk->readonly = 1;
            }
        }
        cur = cur->next;
    }

    if (source == NULL) {
        qemudReportError(server, VIR_ERR_NO_SOURCE, target ? "%s" : NULL, target);
        goto error;
    }
    if (target == NULL) {
        qemudReportError(server, VIR_ERR_NO_TARGET, source ? "%s" : NULL, source);
        goto error;
    }

    if (device &&
        !strcmp((const char *)device, "floppy") &&
        strcmp((const char *)target, "fda") &&
        strcmp((const char *)target, "fdb")) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "Invalid floppy device name: %s", target);
        goto error;
    }
  
    if (device &&
        !strcmp((const char *)device, "cdrom") &&
        strcmp((const char *)target, "hdc")) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "Invalid cdrom device name: %s", target);
        goto error;
    }

    if (device &&
        !strcmp((const char *)device, "cdrom"))
        disk->readonly = 1;

    if ((!device || !strcmp((const char *)device, "disk")) &&
        strcmp((const char *)target, "hda") &&
        strcmp((const char *)target, "hdb") &&
        strcmp((const char *)target, "hdc") &&
        strcmp((const char *)target, "hdd")) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "Invalid harddisk device name: %s", target);
        goto error;
    }

    strncpy(disk->src, (const char *)source, NAME_MAX-1);
    disk->src[NAME_MAX-1] = '\0';

    strncpy(disk->dst, (const char *)target, NAME_MAX-1);
    disk->dst[NAME_MAX-1] = '\0';
    disk->type = typ;

    if (!device)
        disk->device = QEMUD_DISK_DISK;
    else if (!strcmp((const char *)device, "disk"))
        disk->device = QEMUD_DISK_DISK;
    else if (!strcmp((const char *)device, "cdrom"))
        disk->device = QEMUD_DISK_CDROM;
    else if (!strcmp((const char *)device, "floppy"))
        disk->device = QEMUD_DISK_FLOPPY;
    else {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "Invalid device type: %s", device);
        goto error;
    }

    xmlFree(device);
    xmlFree(target);
    xmlFree(source);

    return disk;

 error:
    if (type)
        xmlFree(type);
    if (target)
        xmlFree(target);
    if (source)
        xmlFree(source);
    if (device)
        xmlFree(device);
    free(disk);
    return NULL;
}


/* Parse the XML definition for a network interface */
static struct qemud_vm_net_def *qemudParseInterfaceXML(struct qemud_server *server,
                                                       xmlNodePtr node) {
    struct qemud_vm_net_def *net = calloc(1, sizeof(struct qemud_vm_net_def));
    xmlNodePtr cur;
    xmlChar *macaddr = NULL;
    xmlChar *type = NULL;

    if (!net) {
        qemudReportError(server, VIR_ERR_NO_MEMORY, "net");
        return NULL;
    }

    net->type = QEMUD_NET_USER;

    type = xmlGetProp(node, BAD_CAST "type");
    if (type != NULL) {
        if (xmlStrEqual(type, BAD_CAST "user"))
            net->type = QEMUD_NET_USER;
        else if (xmlStrEqual(type, BAD_CAST "tap"))
            net->type = QEMUD_NET_TAP;
        else if (xmlStrEqual(type, BAD_CAST "server"))
            net->type = QEMUD_NET_SERVER;
        else if (xmlStrEqual(type, BAD_CAST "client"))
            net->type = QEMUD_NET_CLIENT;
        else if (xmlStrEqual(type, BAD_CAST "mcast"))
            net->type = QEMUD_NET_MCAST;
        /*
        else if (xmlStrEqual(type, BAD_CAST "vde"))
          typ = QEMUD_NET_VDE;
        */
        else
            net->type = QEMUD_NET_USER;
        xmlFree(type);
        type = NULL;
    }

    cur = node->children;
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if ((macaddr == NULL) &&
                (xmlStrEqual(cur->name, BAD_CAST "mac"))) {
                macaddr = xmlGetProp(cur, BAD_CAST "address");
            }
        }
        cur = cur->next;
    }

    net->vlan = 0;

    if (macaddr) {
        sscanf((const char *)macaddr, "%02x:%02x:%02x:%02x:%02x:%02x",
               (unsigned int*)&net->mac[0],
               (unsigned int*)&net->mac[1],
               (unsigned int*)&net->mac[2],
               (unsigned int*)&net->mac[3],
               (unsigned int*)&net->mac[4],
               (unsigned int*)&net->mac[5]);

        xmlFree(macaddr);
    }

    return net;
}


/*
 * Parses a libvirt XML definition of a guest, and populates the
 * the qemud_vm struct with matching data about the guests config
 */
static int qemudParseXML(struct qemud_server *server,
                         xmlDocPtr xml,
                         struct qemud_vm *vm) {
    xmlNodePtr root = NULL;
    xmlChar *prop = NULL;
    xmlXPathContextPtr ctxt = NULL;
    xmlXPathObjectPtr obj = NULL;
    char *conv = NULL;
    int i;

    /* Prepare parser / xpath context */
    root = xmlDocGetRootElement(xml);
    if ((root == NULL) || (!xmlStrEqual(root->name, BAD_CAST "domain"))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "incorrect root element");
        goto error;
    }

    ctxt = xmlXPathNewContext(xml);
    if (ctxt == NULL) {
        qemudReportError(server, VIR_ERR_NO_MEMORY, "xmlXPathContext");
        goto error;
    }


    /* Find out what type of QEMU virtualization to use */
    if (!(prop = xmlGetProp(root, BAD_CAST "type"))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "missing domain type attribute");
        goto error;
    }

    if (!strcmp((char *)prop, "qemu"))
        vm->def.virtType = QEMUD_VIRT_QEMU;
    else if (!strcmp((char *)prop, "kqemu"))
        vm->def.virtType = QEMUD_VIRT_KQEMU;
    else if (!strcmp((char *)prop, "kvm"))
        vm->def.virtType = QEMUD_VIRT_KVM;
    else {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "invalid domain type attribute");
        goto error;
    }
    free(prop);
    prop = NULL;


    /* Extract domain name */
    obj = xmlXPathEval(BAD_CAST "string(/domain/name[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        qemudReportError(server, VIR_ERR_NO_NAME, NULL);
        goto error;
    }
    if (strlen((const char *)obj->stringval) >= (QEMUD_MAX_NAME_LEN-1)) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "domain name length too long");
        goto error;
    }
    strcpy(vm->def.name, (const char *)obj->stringval);
    xmlXPathFreeObject(obj);


    /* Extract domain uuid */
    obj = xmlXPathEval(BAD_CAST "string(/domain/uuid[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        /* XXX auto-generate a UUID */
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "missing uuid element");
        goto error;
    }
    if (qemudParseUUID((const char *)obj->stringval, vm->def.uuid) < 0) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "malformed uuid element");
        goto error;
    }
    xmlXPathFreeObject(obj);


    /* Extract domain memory */
    obj = xmlXPathEval(BAD_CAST "string(/domain/memory[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "missing memory element");
        goto error;
    } else {
        conv = NULL;
        vm->def.maxmem = strtoll((const char*)obj->stringval, &conv, 10);
        if (conv == (const char*)obj->stringval) {
            qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "malformed memory information");
            goto error;
        }
    }
    if (obj)
        xmlXPathFreeObject(obj);


    /* Extract domain memory */
    obj = xmlXPathEval(BAD_CAST "string(/domain/currentMemory[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        vm->def.memory = vm->def.maxmem;
    } else {
        conv = NULL;
        vm->def.memory = strtoll((const char*)obj->stringval, &conv, 10);
        if (vm->def.memory > vm->def.maxmem)
            vm->def.memory = vm->def.maxmem;
        if (conv == (const char*)obj->stringval) {
            qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "malformed memory information");
            goto error;
        }
    }
    if (obj)
        xmlXPathFreeObject(obj);

    /* Extract domain vcpu info */
    obj = xmlXPathEval(BAD_CAST "string(/domain/vcpu[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        vm->def.vcpus = 1;
    } else {
        conv = NULL;
        vm->def.vcpus = strtoll((const char*)obj->stringval, &conv, 10);
        if (conv == (const char*)obj->stringval) {
            qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "malformed vcpu information");
            goto error;
        }
    }
    if (obj)
        xmlXPathFreeObject(obj);

    /* See if ACPI feature is requested */
    obj = xmlXPathEval(BAD_CAST "/domain/features/acpi", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
        (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr == 1)) {
        vm->def.features |= QEMUD_FEATURE_ACPI;
    }

    /* Extract OS type info */
    obj = xmlXPathEval(BAD_CAST "string(/domain/os/type[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        qemudReportError(server, VIR_ERR_OS_TYPE, NULL);
        goto error;
    }
    if (strcmp((const char *)obj->stringval, "hvm")) {
        qemudReportError(server, VIR_ERR_OS_TYPE, "%s", obj->stringval);
        goto error;
    }
    strcpy(vm->def.os.type, (const char *)obj->stringval);
    xmlXPathFreeObject(obj);


    obj = xmlXPathEval(BAD_CAST "string(/domain/os/type[1]/@arch)", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        const char *defaultArch = qemudDefaultArch();
        if (strlen(defaultArch) >= (QEMUD_OS_TYPE_MAX_LEN-1)) {
            qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "architecture type too long");
            goto error;
        }
        strcpy(vm->def.os.arch, defaultArch);
    } else {
        if (strlen((const char *)obj->stringval) >= (QEMUD_OS_TYPE_MAX_LEN-1)) {
            qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "architecture type too long");
            goto error;
        }
        strcpy(vm->def.os.arch, (const char *)obj->stringval);
    }
    if (obj)
        xmlXPathFreeObject(obj);

    obj = xmlXPathEval(BAD_CAST "string(/domain/os/type[1]/@machine)", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        const char *defaultMachine = qemudDefaultMachineForArch(vm->def.os.arch);
        if (strlen(defaultMachine) >= (QEMUD_OS_MACHINE_MAX_LEN-1)) {
            qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "machine type too long");
            goto error;
        }
        strcpy(vm->def.os.machine, defaultMachine);
    } else {
        if (strlen((const char *)obj->stringval) >= (QEMUD_OS_MACHINE_MAX_LEN-1)) {
            qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "architecture type too long");
            goto error;
        }
        strcpy(vm->def.os.machine, (const char *)obj->stringval);
    }
    if (obj)
        xmlXPathFreeObject(obj);


    obj = xmlXPathEval(BAD_CAST "string(/domain/os/kernel[1])", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_STRING) &&
        (obj->stringval != NULL) && (obj->stringval[0] != 0)) {
        if (strlen((const char *)obj->stringval) >= (PATH_MAX-1)) {
            qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "kernel path too long");
            goto error;
        }
        strcpy(vm->def.os.kernel, (const char *)obj->stringval);
    }
    if (obj)
        xmlXPathFreeObject(obj);


    obj = xmlXPathEval(BAD_CAST "string(/domain/os/initrd[1])", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_STRING) &&
        (obj->stringval != NULL) && (obj->stringval[0] != 0)) {
        if (strlen((const char *)obj->stringval) >= (PATH_MAX-1)) {
            qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "initrd path too long");
            goto error;
        }
        strcpy(vm->def.os.initrd, (const char *)obj->stringval);
    }
    if (obj)
        xmlXPathFreeObject(obj);


    obj = xmlXPathEval(BAD_CAST "string(/domain/os/cmdline[1])", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_STRING) &&
        (obj->stringval != NULL) && (obj->stringval[0] != 0)) {
        if (strlen((const char *)obj->stringval) >= (PATH_MAX-1)) {
            qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "cmdline arguments too long");
            goto error;
        }
        strcpy(vm->def.os.cmdline, (const char *)obj->stringval);
    }
    if (obj)
        xmlXPathFreeObject(obj);


    /* analysis of the disk devices */
    obj = xmlXPathEval(BAD_CAST "/domain/os/boot", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
        (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr >= 0)) {
        for (i = 0; i < obj->nodesetval->nodeNr && i < QEMUD_MAX_BOOT_DEVS ; i++) {
            prop = xmlGetProp(obj->nodesetval->nodeTab[i], BAD_CAST "dev");
            if (!strcmp((char *)prop, "hd")) {
                vm->def.os.bootDevs[vm->def.os.nBootDevs++] = QEMUD_BOOT_DISK;
            } else if (!strcmp((char *)prop, "fd")) {
                vm->def.os.bootDevs[vm->def.os.nBootDevs++] = QEMUD_BOOT_FLOPPY;
            } else if (!strcmp((char *)prop, "cdrom")) {
                vm->def.os.bootDevs[vm->def.os.nBootDevs++] = QEMUD_BOOT_CDROM;
            } else if (!strcmp((char *)prop, "net")) {
                vm->def.os.bootDevs[vm->def.os.nBootDevs++] = QEMUD_BOOT_NET;
            } else {
                goto error;
            }
        }
    }
    xmlXPathFreeObject(obj);
    if (vm->def.os.nBootDevs == 0) {
        vm->def.os.nBootDevs = 1;
        vm->def.os.bootDevs[0] = QEMUD_BOOT_DISK;
    }


    obj = xmlXPathEval(BAD_CAST "string(/domain/devices/emulator[1])", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_STRING) ||
        (obj->stringval == NULL) || (obj->stringval[0] == 0)) {
        char *tmp = qemudLocateBinaryForArch(server, vm->def.virtType, vm->def.os.arch);
        if (!tmp) {
            goto error;
        }
        strcpy(vm->def.os.binary, tmp);
        free(tmp);
    } else {
        if (strlen((const char *)obj->stringval) >= (PATH_MAX-1)) {
            qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "%s", "emulator path too long");
            goto error;
        }
        strcpy(vm->def.os.binary, (const char *)obj->stringval);
    }
    if (obj)
        xmlXPathFreeObject(obj);

    obj = xmlXPathEval(BAD_CAST "/domain/devices/graphics", ctxt);
    if ((obj == NULL) || (obj->type != XPATH_NODESET) ||
        (obj->nodesetval == NULL) || (obj->nodesetval->nodeNr == 0)) {
        vm->def.graphicsType = QEMUD_GRAPHICS_NONE;
    } else {
        prop = xmlGetProp(obj->nodesetval->nodeTab[0], BAD_CAST "type");
        if (!strcmp((char *)prop, "vnc")) {
            vm->def.graphicsType = QEMUD_GRAPHICS_VNC;
            prop = xmlGetProp(obj->nodesetval->nodeTab[0], BAD_CAST "port");
            if (prop) {
                conv = NULL;
                vm->def.vncPort = strtoll((const char*)prop, &conv, 10);
            } else {
                vm->def.vncPort = -1;
            }
        } else if (!strcmp((char *)prop, "sdl")) {
            vm->def.graphicsType = QEMUD_GRAPHICS_SDL;
        } else {
            qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "Unsupported graphics type %s", prop);
            goto error;
        }
    }

    /* analysis of the disk devices */
    obj = xmlXPathEval(BAD_CAST "/domain/devices/disk", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
        (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr >= 0)) {
        for (i = 0; i < obj->nodesetval->nodeNr; i++) {
            struct qemud_vm_disk_def *disk;
            if (!(disk = qemudParseDiskXML(server, obj->nodesetval->nodeTab[i]))) {
                goto error;
            }
            vm->def.ndisks++;
            disk->next = vm->def.disks;
            vm->def.disks = disk;
        }
    }
    xmlXPathFreeObject(obj);


    /* analysis of the network devices */
    obj = xmlXPathEval(BAD_CAST "/domain/devices/interface", ctxt);
    if ((obj != NULL) && (obj->type == XPATH_NODESET) &&
        (obj->nodesetval != NULL) && (obj->nodesetval->nodeNr >= 0)) {
        for (i = 0; i < obj->nodesetval->nodeNr; i++) {
            struct qemud_vm_net_def *net;
            if (!(net = qemudParseInterfaceXML(server, obj->nodesetval->nodeTab[i]))) {
                goto error;
            }
            vm->def.nnets++;
            net->next = vm->def.nets;
            vm->def.nets = net;
        }
    }
    xmlXPathFreeObject(obj);
    xmlXPathFreeContext(ctxt);

    return 0;

 error:
    if (prop)
        free(prop);
    if (obj)
        xmlXPathFreeObject(obj);
    if (ctxt)
        xmlXPathFreeContext(ctxt);
    return -1;
}


/*
 * Constructs a argv suitable for launching qemu with config defined
 * for a given virtual machine.
 */
int qemudBuildCommandLine(struct qemud_server *server,
                          struct qemud_vm *vm,
                          char ***argv,
                          int *argc) {
    int n = -1, i;
    char memory[50];
    char vcpus[50];
    char boot[QEMUD_MAX_BOOT_DEVS+1];
    struct qemud_vm_disk_def *disk = vm->def.disks;
    struct qemud_vm_net_def *net = vm->def.nets;

    *argc = 1 + /* qemu */
        2 + /* machine type */
        (vm->def.virtType == QEMUD_VIRT_QEMU ? 1 : 0) + /* Disable kqemu */
        2 * vm->def.ndisks + /* disks*/
        (vm->def.nnets > 0 ? (4 * vm->def.nnets) : 2) + /* networks */
        2 + /* memory*/
        2 + /* cpus */
        2 + /* boot device */
        2 + /* monitor */
        (vm->def.features & QEMUD_FEATURE_ACPI ? 0 : 1) + /* acpi */
        (vm->def.os.kernel[0] ? 2 : 0) + /* kernel */
        (vm->def.os.initrd[0] ? 2 : 0) + /* initrd */
        (vm->def.os.cmdline[0] ? 2 : 0) + /* cmdline */
        (vm->def.graphicsType == QEMUD_GRAPHICS_VNC ? 2 :
         (vm->def.graphicsType == QEMUD_GRAPHICS_SDL ? 0 : 1)); /* graphics */

    sprintf(memory, "%d", vm->def.memory/1024);
    sprintf(vcpus, "%d", vm->def.vcpus);

    if (!(*argv = malloc(sizeof(char *) * (*argc +1))))
        goto no_memory;
    if (!((*argv)[++n] = strdup(vm->def.os.binary)))
        goto no_memory;
    if (!((*argv)[++n] = strdup("-M")))
        goto no_memory;
    if (!((*argv)[++n] = strdup(vm->def.os.machine)))
        goto no_memory;
    if (vm->def.virtType == QEMUD_VIRT_QEMU) {
        if (!((*argv)[++n] = strdup("-no-kqemu")))
        goto no_memory;
    }
    if (!((*argv)[++n] = strdup("-m")))
        goto no_memory;
    if (!((*argv)[++n] = strdup(memory)))
        goto no_memory;
    if (!((*argv)[++n] = strdup("-smp")))
        goto no_memory;
    if (!((*argv)[++n] = strdup(vcpus)))
        goto no_memory;

    if (!((*argv)[++n] = strdup("-monitor")))
        goto no_memory;
    if (!((*argv)[++n] = strdup("pty")))
        goto no_memory;

    if (!(vm->def.features & QEMUD_FEATURE_ACPI)) {
    if (!((*argv)[++n] = strdup("-no-acpi")))
        goto no_memory;
    }

    for (i = 0 ; i < vm->def.os.nBootDevs ; i++) {
        switch (vm->def.os.bootDevs[i]) {
        case QEMUD_BOOT_CDROM:
            boot[i] = 'd';
            break;
        case QEMUD_BOOT_FLOPPY:
            boot[i] = 'a';
            break;
        case QEMUD_BOOT_DISK:
            boot[i] = 'c';
            break;
        case QEMUD_BOOT_NET:
            boot[i] = 'n';
            break;
        default:
            boot[i] = 'c';
            break;
        }
    }
    boot[vm->def.os.nBootDevs] = '\0';
    if (!((*argv)[++n] = strdup("-boot")))
        goto no_memory;
    if (!((*argv)[++n] = strdup(boot)))
        goto no_memory;

    if (vm->def.os.kernel[0]) {
        if (!((*argv)[++n] = strdup("-kernel")))
            goto no_memory;
        if (!((*argv)[++n] = strdup(vm->def.os.kernel)))
            goto no_memory;
    }
    if (vm->def.os.initrd[0]) {
        if (!((*argv)[++n] = strdup("-initrd")))
            goto no_memory;
        if (!((*argv)[++n] = strdup(vm->def.os.initrd)))
            goto no_memory;
    }
    if (vm->def.os.cmdline[0]) {
        if (!((*argv)[++n] = strdup("-append")))
            goto no_memory;
        if (!((*argv)[++n] = strdup(vm->def.os.cmdline)))
            goto no_memory;
    }

    while (disk) {
        char dev[NAME_MAX];
        char file[PATH_MAX];
        if (!strcmp(disk->dst, "hdc") &&
            disk->device == QEMUD_DISK_CDROM)
            snprintf(dev, NAME_MAX, "-%s", "cdrom");
        else
            snprintf(dev, NAME_MAX, "-%s", disk->dst);
        snprintf(file, PATH_MAX, "%s", disk->src);

        if (!((*argv)[++n] = strdup(dev)))
            goto no_memory;
        if (!((*argv)[++n] = strdup(file)))
            goto no_memory;

        disk = disk->next;
    }

    if (!net) {
        if (!((*argv)[++n] = strdup("-net")))
            goto no_memory;
        if (!((*argv)[++n] = strdup("none")))
            goto no_memory;
    } else {
        while (net) {
            char nic[3+1+7+1+17+1];
            sprintf(nic, "nic,macaddr=%02x:%02x:%02x:%02x:%02x:%02x",
                    net->mac[0], net->mac[1],
                    net->mac[2], net->mac[3],
                    net->mac[4], net->mac[5]);

            if (!((*argv)[++n] = strdup("-net")))
                goto no_memory;
            if (!((*argv)[++n] = strdup(nic)))
                goto no_memory;
            if (!((*argv)[++n] = strdup("-net")))
                goto no_memory;
            /* XXX don't hardcode user */
            if (!((*argv)[++n] = strdup("user")))
                goto no_memory;

            net = net->next;
        }
    }

    if (vm->def.graphicsType == QEMUD_GRAPHICS_VNC) {
        char port[10];
        snprintf(port, 10, "%d", vm->def.vncActivePort - 5900);
        if (!((*argv)[++n] = strdup("-vnc")))
            goto no_memory;
        if (!((*argv)[++n] = strdup(port)))
            goto no_memory;
    } else if (vm->def.graphicsType == QEMUD_GRAPHICS_NONE) {
        if (!((*argv)[++n] = strdup("-nographic")))
            goto no_memory;
    } else {
        /* SDL is the default. no args needed */
    }

    (*argv)[++n] = NULL;

    return 0;

 no_memory:
    if (argv) {
        for (i = 0 ; i < n ; i++)
            free(argv[i]);
        free(argv);
    }
    qemudReportError(server, VIR_ERR_NO_MEMORY, "argv");
    return -1;
}

/* Free all memory associated with a struct qemud_vm object */
void qemudFreeVM(struct qemud_vm *vm) {
    struct qemud_vm_disk_def *disk = vm->def.disks;
    struct qemud_vm_net_def *net = vm->def.nets;

    while (disk) {
        struct qemud_vm_disk_def *prev = disk;
        disk = disk->next;
        free(prev);
    }
    while (net) {
        struct qemud_vm_net_def *prev = net;
        net = net->next;
        free(prev);
    }

    free(vm);
}

/* Build up a fully qualified path for a config file to be
 * associated with a persistent guest */
static
int qemudMakeConfigPath(struct qemud_server *server,
                        const char *name,
                        const char *ext,
                        char *buf,
                        unsigned int buflen) {
    if ((strlen(server->configDir) + 1 + strlen(name) + (ext ? strlen(ext) : 0) + 1) > buflen)
        return -1;

    strcpy(buf, server->configDir);
    strcat(buf, "/");
    strcat(buf, name);
    if (ext)
        strcat(buf, ext);
    return 0;
}


/* Save a guest's config data into a persistent file */
static int qemudSaveConfig(struct qemud_server *server,
                           struct qemud_vm *vm) {
    char *xml;
    int fd = -1, ret = -1;
    int towrite;
    struct stat sb;

    if (!(xml = qemudGenerateXML(server, vm))) {
        return -1;
    }

    if (stat(server->configDir, &sb) < 0) {
        if (errno == ENOENT) {
            if (mkdir(server->configDir, 0700) < 0) {
                qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                                 "cannot create config directory %s",
                                 server->configDir);
                return -1;
            }
        } else {
            qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                             "cannot stat config directory %s",
                             server->configDir);
            return -1;
        }
    } else if (!S_ISDIR(sb.st_mode)) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "config directory %s is not a directory",
                         server->configDir);
        return -1;
    }

    if ((fd = open(vm->configFile,
                   O_WRONLY | O_CREAT | O_TRUNC,
                   S_IRUSR | S_IWUSR )) < 0) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "cannot create config file %s",
                         vm->configFile);
        goto cleanup;
    }

    towrite = strlen(xml);
    if (write(fd, xml, towrite) != towrite) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "cannot write config file %s",
                         vm->configFile);
        goto cleanup;
    }

    if (close(fd) < 0) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                         "cannot save config file %s",
                         vm->configFile);
        goto cleanup;
    }

    ret = 0;

 cleanup:
    if (fd != -1)
        close(fd);

    free(xml);

    return ret;
}


/* Create a qemud_vm instance, populating it based on the data
 * in a libvirt XML document describing the guest */
struct qemud_vm *qemudLoadConfigXML(struct qemud_server *server,
                                    const char *file,
                                    const char *doc,
                                    int save) {
    struct qemud_vm *vm = NULL;
    xmlDocPtr xml;

    if (!(xml = xmlReadDoc(BAD_CAST doc, file ? file : "domain.xml", NULL,
                           XML_PARSE_NOENT | XML_PARSE_NONET |
                           XML_PARSE_NOERROR | XML_PARSE_NOWARNING))) {
        qemudReportError(server, VIR_ERR_XML_ERROR, NULL);
        return NULL;
    }

    if (!(vm = calloc(1, sizeof(struct qemud_vm)))) {
        qemudReportError(server, VIR_ERR_NO_MEMORY, "vm");
        return NULL;
    }

    vm->stdout = -1;
    vm->stderr = -1;
    vm->monitor = -1;
    vm->pid = -1;
    vm->def.id = -1;

    if (qemudParseXML(server, xml, vm) < 0) {
        xmlFreeDoc(xml);
        qemudFreeVM(vm);
        return NULL;
    }
    xmlFreeDoc(xml);

    if (qemudFindVMByUUID(server, vm->def.uuid)) {
        qemudReportError(server, VIR_ERR_DOM_EXIST, vm->def.name);
        qemudFreeVM(vm);
        return NULL;
    }

    if (qemudFindVMByName(server, vm->def.name)) {
        qemudReportError(server, VIR_ERR_DOM_EXIST, vm->def.name);
        qemudFreeVM(vm);
        return NULL;
    }

    if (file) {
        strncpy(vm->configFile, file, PATH_MAX);
        vm->configFile[PATH_MAX-1] = '\0';
    } else {
        if (save) {
            if (qemudMakeConfigPath(server, vm->def.name, ".xml", vm->configFile, PATH_MAX) < 0) {
                qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                                 "cannot construct config file path");
                qemudFreeVM(vm);
                return NULL;
            }

            if (qemudSaveConfig(server, vm) < 0) {
                qemudReportError(server, VIR_ERR_INTERNAL_ERROR,
                                 "cannot save config file for guest");
                qemudFreeVM(vm);
                return NULL;
            }
        } else {
            vm->configFile[0] = '\0';
        }
    }

    return vm;
}


/* Load a guest from its persistent config file */
static void qemudLoadConfig(struct qemud_server *server,
                            const char *file) {
    FILE *fh;
    struct stat st;
    struct qemud_vm *vm;
    char xml[QEMUD_MAX_XML_LEN];
    int ret;

    if (!(fh = fopen(file, "r"))) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "cannot open guest config file %s", file);
        return;
    }

    if (fstat(fileno(fh), &st) < 0) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "cannot stat config file %s", file);
        goto cleanup;
    }

    if (st.st_size >= QEMUD_MAX_XML_LEN) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "guest config too large in file %s", file);
        goto cleanup;
    }

    if ((ret = fread(xml, st.st_size, 1, fh)) != 1) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "cannot read config file %s", file);
        goto cleanup;
    }
    xml[st.st_size] = '\0';

    if ((vm = qemudLoadConfigXML(server, file, xml, 1))) {
        vm->next = server->inactivevms;
        server->inactivevms = vm;
        server->ninactivevms++;
    }
  
 cleanup:
    fclose(fh);
}


/* Scan for all guest config files */
int qemudScanConfigs(struct qemud_server *server) {
    DIR *dir;
    struct dirent *entry;

    if (!(dir = opendir(server->configDir))) {
        if (errno == ENOENT)
            return 0;
        return -1;
    }

    while ((entry = readdir(dir))) {
        char file[PATH_MAX];
        if (entry->d_name[0] == '.')
            continue;

        if (qemudMakeConfigPath(server, entry->d_name, NULL, file, PATH_MAX) < 0)
            continue;

        qemudLoadConfig(server, file);
    }

    closedir(dir);
 
    return 0;
}


/* Simple grow-on-demand string buffer */
/* XXX re-factor to shared library */
struct qemudBuffer {
    char *data;
    int len;
    int used;
};

static
int qemudBufferAdd(struct qemudBuffer *buf, const char *str) {
    int need = strlen(str);
  
    if ((need+1) > (buf->len-buf->used)) {
        return -1;
    }
  
    memcpy(buf->data + buf->used, str, need+1);
    buf->used += need;

    return 0;
}


static
int qemudBufferPrintf(struct qemudBuffer *buf,
                      const char *format, ...) {
    int size, count;
    va_list locarg, argptr;

    if ((format == NULL) || (buf == NULL)) {
        return -1;
    }
    size = buf->len - buf->used - 1;
    va_start(argptr, format);
    va_copy(locarg, argptr);

    if ((count = vsnprintf(&buf->data[buf->used],
                           size,
                           format,
                           locarg)) >= size) {
        return -1;
    }
    va_end(locarg);
    buf->used += count;

    buf->data[buf->used] = '\0';
    return 0;
}

/* Generate an XML document describing the guest's configuration */
char *qemudGenerateXML(struct qemud_server *server, struct qemud_vm *vm) {
    struct qemudBuffer buf;
    unsigned char *uuid;
    struct qemud_vm_disk_def *disk;
    struct qemud_vm_net_def *net;
    const char *type = NULL;
    int n;

    buf.len = QEMUD_MAX_XML_LEN;
    buf.used = 0;
    buf.data = malloc(buf.len);

    switch (vm->def.virtType) {
    case QEMUD_VIRT_QEMU:
        type = "qemu";
        break;
    case QEMUD_VIRT_KQEMU:
        type = "kqemu";
        break;
    case QEMUD_VIRT_KVM:
        type = "kvm";
        break;
    }
    if (!type) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "unexpected domain type %d", vm->def.virtType);
        goto cleanup;
    }

    if (vm->def.id >= 0) {
        if (qemudBufferPrintf(&buf, "<domain type='%s' id='%d'>\n", type, vm->def.id) < 0)
            goto no_memory;
    } else {
        if (qemudBufferPrintf(&buf, "<domain type='%s'>\n", type) < 0)
            goto no_memory;
    }

    if (qemudBufferPrintf(&buf, "  <name>%s</name>\n", vm->def.name) < 0)
        goto no_memory;

    uuid = vm->def.uuid;
    if (qemudBufferPrintf(&buf, "  <uuid>%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x</uuid>\n",
                          uuid[0], uuid[1], uuid[2], uuid[3],
                          uuid[4], uuid[5], uuid[6], uuid[7],
                          uuid[8], uuid[9], uuid[10], uuid[11],
                          uuid[12], uuid[13], uuid[14], uuid[15]) < 0)
        goto no_memory;
    if (qemudBufferPrintf(&buf, "  <memory>%d</memory>\n", vm->def.maxmem) < 0)
        goto no_memory;
    if (qemudBufferPrintf(&buf, "  <currentMemory>%d</currentMemory>\n", vm->def.memory) < 0)
        goto no_memory;
    if (qemudBufferPrintf(&buf, "  <vcpu>%d</vcpu>\n", vm->def.vcpus) < 0)
        goto no_memory;

    if (qemudBufferAdd(&buf, "  <os>\n") < 0)
        goto no_memory;

    if (vm->def.virtType == QEMUD_VIRT_QEMU) {
        if (qemudBufferPrintf(&buf, "    <type arch='%s' machine='%s'>%s</type>\n",
                              vm->def.os.arch, vm->def.os.machine, vm->def.os.type) < 0)
            goto no_memory;
    } else {
        if (qemudBufferPrintf(&buf, "    <type>%s</type>\n", vm->def.os.type) < 0)
            goto no_memory;
    }

    if (vm->def.os.kernel[0])
        if (qemudBufferPrintf(&buf, "    <kernel>%s</kernel>\n", vm->def.os.kernel) < 0)
            goto no_memory;
    if (vm->def.os.initrd[0])
        if (qemudBufferPrintf(&buf, "    <initrd>%s</initrd>\n", vm->def.os.initrd) < 0)
            goto no_memory;
    if (vm->def.os.cmdline[0])
        if (qemudBufferPrintf(&buf, "    <cmdline>%s</cmdline>\n", vm->def.os.cmdline) < 0)
            goto no_memory;

    if (vm->def.features & QEMUD_FEATURE_ACPI) {
        if (qemudBufferAdd(&buf, "  <features>\n") < 0)
            goto no_memory;
        if (qemudBufferAdd(&buf, "    <acpi>\n") < 0)
            goto no_memory;
        if (qemudBufferAdd(&buf, "  </features>\n") < 0)
            goto no_memory;
    }


    for (n = 0 ; n < vm->def.os.nBootDevs ; n++) {
        const char *boottype = "hd";
        switch (vm->def.os.bootDevs[n]) {
        case QEMUD_BOOT_FLOPPY:
            boottype = "fd";
            break;
        case QEMUD_BOOT_DISK:
            boottype = "hd";
            break;
        case QEMUD_BOOT_CDROM:
            boottype = "cdrom";
            break;
        case QEMUD_BOOT_NET:
            boottype = "net";
            break;
        }
        if (qemudBufferPrintf(&buf, "    <boot dev='%s'/>\n", boottype) < 0)
            goto no_memory;
    }

    if (qemudBufferAdd(&buf, "  </os>\n") < 0)
        goto no_memory;

    if (qemudBufferAdd(&buf, "  <devices>\n") < 0)
        goto no_memory;

    if (qemudBufferPrintf(&buf, "    <emulator>%s</emulator>\n", vm->def.os.binary) < 0)
        goto no_memory;

    disk = vm->def.disks;
    while (disk) {
        const char *types[] = {
            "block",
            "file",
        };
        const char *typeAttrs[] = {
            "dev",
            "file",
        };
        const char *devices[] = {
            "disk",
            "cdrom",
            "floppy",
        };
        if (qemudBufferPrintf(&buf, "    <disk type='%s' device='%s'>\n",
                              types[disk->type], devices[disk->device]) < 0)
            goto no_memory;

        if (qemudBufferPrintf(&buf, "      <source %s='%s'/>\n", typeAttrs[disk->type], disk->src) < 0)
            goto no_memory;

        if (qemudBufferPrintf(&buf, "      <target dev='%s'/>\n", disk->dst) < 0)
            goto no_memory;

        if (disk->readonly)
            if (qemudBufferAdd(&buf, "      <readonly/>\n") < 0)
                goto no_memory;

        if (qemudBufferPrintf(&buf, "    </disk>\n") < 0)
            goto no_memory;

        disk = disk->next;
    }

    net = vm->def.nets;
    disk = vm->def.disks;
    while (disk) {
        const char *types[] = {
            "user",
            "tap",
            "server",
            "client",
            "mcast",
            "vde",
        };
        if (qemudBufferPrintf(&buf, "    <interface type='%s'>\n",
                              types[net->type]) < 0)
            goto no_memory;

        if (qemudBufferPrintf(&buf, "      <mac address='%02x:%02x:%02x:%02x:%02x:%02x'/>\n",
                              net->mac[0], net->mac[1], net->mac[2],
                              net->mac[3], net->mac[4], net->mac[5]) < 0)
            goto no_memory;

        if (qemudBufferPrintf(&buf, "    </interface>\n") < 0)
            goto no_memory;

        disk = disk->next;
    }

    if (vm->def.graphicsType == QEMUD_GRAPHICS_VNC) {
        if (vm->def.vncPort) {
            qemudBufferPrintf(&buf, "    <graphics type='vnc' port='%d'/>\n",
                              vm->def.id == -1 ? vm->def.vncPort : vm->def.vncActivePort);
        } else {
            qemudBufferPrintf(&buf, "    <graphics type='vnc'/>\n");
        }
    }

    if (qemudBufferAdd(&buf, "  </devices>\n") < 0)
        goto no_memory;


    if (qemudBufferAdd(&buf, "</domain>\n") < 0)
        goto no_memory;

    return buf.data;

 no_memory:
    qemudReportError(server, VIR_ERR_NO_MEMORY, "xml");
 cleanup:
    free(buf.data);
    return NULL;
}


int qemudDeleteConfigXML(struct qemud_server *server, struct qemud_vm *vm) {
    if (!vm->configFile[0]) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "no config file for guest %s", vm->def.name);
        return -1;
    }

    if (unlink(vm->configFile) < 0) {
        qemudReportError(server, VIR_ERR_INTERNAL_ERROR, "cannot remove config for guest %s", vm->def.name);
        return -1;
    }

    vm->configFile[0] = '\0';

    return 0;
}


/*
 * Local variables:
 *  indent-tabs-mode: nil
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
