/*****************************************************************************
 * opensaf.c: VLM interface plugin
 *****************************************************************************
 * Copyright (C) 2000-2006 the VideoLAN team
 * $Id: 442c580877122ac1eab6e0c209be53ef83404ce0 $
 *
 * Authors: Simon Latapie <garf@videolan.org>
 *          Laurent Aimar <fenrir@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_input.h>

#include <stdbool.h>

#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
/*
#ifdef HAVE_UNISTD_H
#   include <unistd.h>
#endif
*/
#ifdef HAVE_POLL
#   include <poll.h>
#endif

#include <vlc_network.h>
#include <vlc_url.h>
#include <vlc_vlm.h>

#include <saAmf.h>
#include <saCkpt.h>

/*includes for tracing*/
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <lttng/tracef.h>


/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );
static char *MessageToString( vlm_message_t *, int );

#if 0
#define TELNETHOST_TEXT N_( "Host" )
#define TELNETHOST_LONGTEXT N_( "This is the host on which the " \
    "interface will listen. It defaults to all network interfaces (0.0.0.0)." \
    " If you want this interface to be available only on the local " \
    "machine, enter \"127.0.0.1\"." )
#define TELNETPORT_TEXT N_( "Port" )
#define TELNETPORT_LONGTEXT N_( "This is the TCP port on which this " \
    "interface will listen. It defaults to 4212." )
#define TELNETPORT_DEFAULT 4212
#define TELNETPWD_TEXT N_( "Password" )
#define TELNETPWD_LONGTEXT N_( "A single administration password is used " \
    "to protect this interface. The default value is \"admin\"." )
#define TELNETPWD_DEFAULT "admin"
#endif

vlc_module_begin ()
    set_shortname( "OpenSaf" )
    set_category( CAT_INTERFACE )
    set_subcategory( SUBCAT_INTERFACE_CONTROL )
//     add_string( "telnet-host", "localhost", NULL, TELNETHOST_TEXT,
//                  TELNETHOST_LONGTEXT, true )
//     add_integer( "telnet-port", TELNETPORT_DEFAULT, NULL, TELNETPORT_TEXT,
//                  TELNETPORT_LONGTEXT, true )
//     add_password( "telnet-password", TELNETPWD_DEFAULT, NULL, TELNETPWD_TEXT,
//                 TELNETPWD_LONGTEXT, true )
    set_description( N_("VLM High Availability control interface") )
    add_category_hint( "VLM", NULL, false )
    set_capability( "interface", 0 )
    set_callbacks( Open , Close )
vlc_module_end ()

#define VLC_CKPT_NAME "safCkpt=VLC_Ckpt,safApp=safCkptService"
#define PLAYLIST_SECTION  "pl"
#define CLIENTS_SECTION   "cl"
#define POSITIONS_SECTION "ps"

#define USER_CONFIG "new aa broadcast enabled\n"\
"setup aa input \"dvdsimple:///dev/dvd1@3\"\n"\
"setup aa output #rtp{dst=239.255.100.100,port=5004,ttl=2}\n"\
"setup aa option ttl=1\n"\
"setup aa option no-sout-rtp-sap\n"\
"setup aa option no-sout-standard-sap\n"\
"setup aa option sout-keep\n"\
"setup aa option dvdread-caching=300\n"

/*****************************************************************************
 * Local prototypes.
 *****************************************************************************/
static void Run( intf_thread_t * );

static void vlc_amf_csi_set_callback(SaInvocationT, 
             const SaNameT *,
             SaAmfHAStateT,
             SaAmfCSIDescriptorT);

/* 'CSI Remove' callback that is registered with AMF */
static void vlc_amf_csi_remove_callback(SaInvocationT, 
          const SaNameT *,
          const SaNameT *,
          SaAmfCSIFlagsT);

/* 'Component Terminate' callback that is registered with AMF */
static void vlc_amf_comp_terminate_callback(SaInvocationT, const SaNameT *);


// typedef struct
// {
//     int        i_mode; /* read or write */
//     int        fd;
//     char       buffer_read[1000]; // 1000 byte per command should be sufficient
//     char      *buffer_write;
//     char      *p_buffer_read;
//     char      *p_buffer_write; // the position in the buffer
//     int        i_buffer_write; // the number of byte we still have to send
//     int        i_tel_cmd; // for specific telnet commands
// 
// } telnet_client_t;
// 
// static char *MessageToString( vlm_message_t *, int );
// static void Write_message( telnet_client_t *, vlm_message_t *, const char *, int );

static intf_thread_t *gl_p_intf;

struct intf_sys_t
{
   SaAmfHandleT    amf_hdl;
   SaSelectionObjectT amf_sel_obj;
   SaNameT     amf_comp_name;
   SaNameT     amf_csi_name;
   SaCkptHandleT   ckpt_hdl;
   SaCkptCheckpointHandleT  checkpoint;
   vlm_t           *mediatheque;
   bool      is_active;
   volatile bool   need_pl_checkpoint;
};

SaAmfCallbacksT    reg_callback_set = {
  .saAmfCSISetCallback = vlc_amf_csi_set_callback,
  .saAmfCSIRemoveCallback = vlc_amf_csi_remove_callback,
  .saAmfComponentTerminateCallback = vlc_amf_comp_terminate_callback,
};

const SaCkptCheckpointCreationAttributesT ckpt_attrs = {
  .creationFlags = SA_CKPT_WR_ACTIVE_REPLICA | SA_CKPT_CHECKPOINT_COLLOCATED,
  .checkpointSize = 10000000,
  .retentionDuration = SA_TIME_MAX,
  .maxSections = 3,
  .maxSectionSize = 10000000,
  .maxSectionIdSize = 2,
};

/* Macro to retrieve the AMF version */
#define m_AMF_VER_GET \
{ \
   .releaseCode = 'B', \
   .majorVersion = 0x01, \
   .minorVersion = 0x01, \
};

#define m_CKPT_VER_GET \
{ \
   .releaseCode = 'B', \
   .majorVersion = 0x02, \
   .minorVersion = 0x00, \
};

/* Canned strings for HA State */
static const char *ha_state_str[] =
{
  "None",
  "Active",    /* SA_AMF_HA_ACTIVE       */
  "Standby",   /* SA_AMF_HA_STANDBY      */
  "Quiesced",  /* SA_AMF_HA_QUIESCED     */
  "Quiescing"  /* SA_AMF_HA_QUIESCING    */
};

/* Canned strings for CSI Flags */
const char *csi_flag_str[] =
{
  "None",
  "Add One",    /* SA_AMF_CSI_ADD_ONE    */
  "Target One", /* SA_AMF_CSI_TARGET_ONE */
  "None",
  "Target All", /* SA_AMF_CSI_TARGET_ALL */
};

#define SaName( name ) { sizeof( name ) -1, name }

SaNameT ckpt_name = SaName(VLC_CKPT_NAME);
SaCkptSectionIdT playlist_section = SaName(PLAYLIST_SECTION),
    clients_section = SaName(CLIENTS_SECTION),
    positions_section = SaName(POSITIONS_SECTION);

char vlm_config[] = USER_CONFIG;

/*****************************************************************************
 * Open: initialize dummy interface
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    intf_thread_t *p_intf = (intf_thread_t*) p_this;
    intf_sys_t    *p_sys;
    vlm_t *mediatheque;
    SaVersionT ver = m_AMF_VER_GET;
    SaAisErrorT rc;
    int err = VLC_EGENERIC;

    if( !(mediatheque = vlm_New( p_intf )) )
    {
        msg_Err( p_intf, "cannot start VLM" );
        return VLC_EGENERIC;
    }else{
      tracef("Starting VLM");
    }

    msg_Info( p_intf, "using the OpenSaf interface plugin..." );

    p_intf->p_sys = p_sys = calloc( 1, sizeof( intf_sys_t ) );
    if( !p_sys )
    {
        err = VLC_ENOMEM;
  goto ERR_open_media;
    }else{
      tracef("Media open successful");
    }
    
    rc = saAmfInitialize(&p_sys->amf_hdl, &reg_callback_set, &ver);
    if( SA_AIS_OK != rc )
    {
        msg_Err( p_intf, "cannot get handle to AMF" );
        goto ERR_open_psys;
    }else{
      tracef("saAmfInitialize successful");
    }
    
    rc = saAmfSelectionObjectGet(p_sys->amf_hdl, &p_sys->amf_sel_obj);
    if (SA_AIS_OK != rc) {
  msg_Err( p_intf, "saAmfSelectionObjectGet FAILED %u", rc);
  goto ERR_open_AMF;
    }else{
      tracef("saAmfSelectionObjectGet successful; selection object returned: %u", rc);
    }

    ver = (SaVersionT ) m_CKPT_VER_GET;
    rc = saCkptInitialize(&p_sys->ckpt_hdl, NULL, &ver);
    if( SA_AIS_OK != rc )
    {
        msg_Err( p_intf, "cannot get handle to Ckpt" );
        goto ERR_open_AMF;
    }else{
      tracef("Got handle to Ckpt");
    }
    
    rc = saAmfComponentNameGet(p_sys->amf_hdl, &p_sys->amf_comp_name);
    if (SA_AIS_OK != rc) {
  msg_Err( p_intf,  "saAmfComponentNameGet FAILED %u", rc);
  goto ERR_open_CKPT;
    }else{
      tracef("saAmfComponentNameGet successful: component name returned: %u", rc);
    }

    msg_Info( p_intf, "Component Name Get Successful !!!");
    msg_Info( p_intf, "\tCompName: %s", p_sys->amf_comp_name.value);
    tracef("Component Name Get Successful !\tCompName: %s", p_sys->amf_comp_name.value);

    /*#########################################################################
      Demonstrating the use of saAmfComponentRegister()
    #########################################################################*/

    rc = saAmfComponentRegister(p_sys->amf_hdl, &p_sys->amf_comp_name, 0);
    if (SA_AIS_OK != rc) {
      msg_Err( p_intf, "saAmfComponentRegister FAILED %u", rc);
      goto ERR_open_CKPT;
    }else{
      tracef("saAmfComponentRegister successful; returned code: %u", rc);
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);

    msg_Info( p_intf, "Component Registered !!! %lld.%lld", tv.tv_sec, tv.tv_usec );
    tracef("Component Registered at %lld.%lld", tv.tv_sec, tv.tv_usec);
    msg_Info( p_intf,
              "OpenSaf interface started");

    p_intf->p_sys->mediatheque = mediatheque;
    p_intf->pf_run = Run;

    return VLC_SUCCESS;
    
  ERR_open_CKPT:
    saCkptFinalize(p_sys->ckpt_hdl);
    
  ERR_open_AMF:
    saAmfFinalize(p_sys->amf_hdl);
    
  ERR_open_psys:
    free( p_sys );
    
  ERR_open_media:
    vlm_Delete( mediatheque );
    
    return err;
}

static void perform_checkpoint( intf_thread_t *p_intf ) {
    SaCkptIOVectorElementT writeVector[3];
    intf_sys_t     *p_sys = p_intf->p_sys;
    vlm_message_t *message = NULL;
    char * playlist = NULL;
    size_t length;
    int vector_count = 1;
    SaUint32T erroneousVectorIndex;
    SaAisErrorT rc;
    static int counter = 0;

    if (p_sys->is_active && p_sys->checkpoint) {
  // perform checkpoint

      if (p_sys->need_pl_checkpoint) {
      p_sys->need_pl_checkpoint = false;

      msg_Info( p_intf, "Doing Playlist checkpoint");
      //tracef("Doing Playlist checkpoint");
      vlm_ExecuteCommand( p_sys->mediatheque, "export\n", &message );
      playlist = MessageToString(message, 0);

      length = strlen(playlist)+1;
      writeVector[1].sectionId = playlist_section;
      writeVector[1].dataOffset = 0;
      writeVector[1].dataSize = sizeof(length);
      writeVector[1].dataBuffer = &length;
      writeVector[2].sectionId = playlist_section;
      writeVector[2].dataOffset = sizeof(length);
      writeVector[2].dataSize = length;
      writeVector[2].dataBuffer = playlist;
      vector_count = 3;
      vlm_MessageDelete( message );
      message = NULL;
  }

  msg_Info( p_intf, "Going for chekpoint.");
  vlm_ExecuteCommand( p_sys->mediatheque, "show_pos\n", &message );
  if (message) {
      msg_Err( p_intf, "Got message back from show_pos\n");
      vlm_MessageDelete( message );
  }
      msg_Info( p_intf, "Local save.");
      //tracef("Local save");
  vlm_GetCkpt( p_sys->mediatheque, &writeVector[0].dataBuffer , &writeVector[0].dataSize );
      msg_Info( p_intf, "Fetch done.");
      //tracef("Fetch done.");
  writeVector[0].sectionId = positions_section;
  writeVector[0].dataOffset = 0;

  rc = saCkptCheckpointWrite(p_sys->checkpoint, writeVector, vector_count, &erroneousVectorIndex);
  if(rc == SA_AIS_OK){
        msg_Info(  p_intf, "Checkpoint" );
        //tracef("Checkpointed");
      }
  else
      msg_Err( p_intf, "Checkpoint failed (vector %u)! %u - section %*s, size %d at offset %d", erroneousVectorIndex, rc, writeVector[erroneousVectorIndex].sectionId.idLen, writeVector[erroneousVectorIndex].sectionId.id, writeVector[erroneousVectorIndex].dataSize, writeVector[erroneousVectorIndex].dataOffset );
  if (playlist)
      free(playlist);
  if (++counter > 10) {
      vlm_ExecuteCommand( p_sys->mediatheque, "show\n", &message );
      char * psz_message = MessageToString(message, 0);
      msg_Info(  p_intf, "%s", psz_message);
      free(psz_message);
      vlm_MessageDelete( message );
      message = NULL;
      counter = 0;
  }
    }
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    intf_thread_t *p_intf = (intf_thread_t*)p_this;
    intf_sys_t    *p_sys  = p_intf->p_sys;

    msg_Info( p_intf, "Close.");

    //if ( p_sys->amf_comp_name.length )
    //  saAmfComponentUnregister(p_sys->amf_hdl, &p_sys->amf_comp_name, 0);

    saCkptFinalize(p_sys->ckpt_hdl);
    saAmfFinalize(p_sys->amf_hdl);

    vlm_Delete( p_sys->mediatheque );
    tracef("Closing VLM");
    free( p_sys );
}

/*****************************************************************************
 * Run: main loop
 *****************************************************************************/
static void Run( intf_thread_t *p_intf )
{
    intf_sys_t     *p_sys = p_intf->p_sys;
    SaAisErrorT rc;
    struct pollfd fds[3];

//     /* FIXME: make sure config_* is cancel-safe */
//     psz_password = var_InheritString( p_intf, "telnet-password" );
//     vlc_cleanup_push( free, psz_password );

    fds[0].fd = p_sys->amf_sel_obj;
    fds[0].events = POLLIN;

    struct timeval tv;
    gettimeofday(&tv, NULL);

    msg_Info( p_intf, "Waiting for command. %lld.%lld", tv.tv_sec, tv.tv_usec );
    tracef("Waiting for command. %lld.%lld", tv.tv_sec, tv.tv_usec);
    while (1) {
      int res = poll(fds, 1, 1000);

  if (res == -1) {
      if (errno == EINTR)
         continue;
      else {
        msg_Err( p_intf, "poll FAILED - %s", strerror(errno));
    libvlc_Quit( p_intf->p_libvlc );
    break;
      }
  }
  if (fds[0].revents & POLLIN) {
      msg_Info( p_intf, "Command Received");
      gl_p_intf = p_intf;
      rc = saAmfDispatch(p_sys->amf_hdl, SA_DISPATCH_ONE);
          if (SA_AIS_OK != rc) {
      msg_Err( p_intf, "saAmfDispatch FAILED %u", rc);
  //  saAmfComponentUnregister(p_sys->amf_hdl, &p_sys->amf_comp_name, 0);
      libvlc_Quit( p_intf->p_libvlc );
    break;
            }else{
              tracef("saAmfDispatch successful, handle returned: %u", rc);
            }
  }
  if (p_sys->is_active)
      perform_checkpoint(p_intf);
    }

//     vlm_message_t *message;
// 
//     /* create a standard string */
//     *cl->p_buffer_read = '\0';
// 
//     vlm_ExecuteCommand( p_sys->mediatheque, cl->buffer_read,
//      &message );
//     vlm_MessageDelete( message );
}


/****************************************************************************
  Name          : avsv_amf_csi_set_callback
 
  Description   : This routine is a callback to set (add/modify) the HA state
      of a CSI (or all the CSIs) that is newly/already assigned 
      to the component. It is specified as a part of AMF 
      initialization. It demonstrates the use of following 
      AMF APIs:
@     d) saAmfComponentUnregister()
      e) saAmfFinalize()

 
  Arguments     : inv       - particular invocation of this callback function
      comp_name - ptr to the component name
      ha_state  - ha state to be assumed by the CSI (or all the 
            CSIs)
      csi_desc  - CSI descriptor

  Return Values : None.
 
  Notes         : This routine starts health check on the active entity. If
      the CSI transitions from standby to active, the demo is 
      considered over & then the usage of 'Component Unregister' &
      'AMF Finalize' is illustrated.
******************************************************************************/
void vlc_amf_csi_set_callback(SaInvocationT       inv, 
             const SaNameT       *comp_name,
             SaAmfHAStateT       ha_state,
             SaAmfCSIDescriptorT csi_desc)
{
  char  seek_str[256];
  intf_sys_t    *p_sys  = gl_p_intf->p_sys;
  SaAisErrorT      rc = 0;
    vlm_message_t *message;
    int64_t pos;
    SaCkptIOVectorElementT  vector;
    SaUint32T erroneousVectorIndex;
    size_t section_length;
    int component_module_pid = getpid();
    int component_pid = getppid();

  struct timeval tv;
  gettimeofday(&tv, NULL);
  msg_Info( gl_p_intf, "Dispatched 'CSI Set' Callback for Component: '%s', %lld.%lld", comp_name->value, tv.tv_sec, tv.tv_usec);
  //tracef("-- | Dispatched 'CSI Set' Callback for Component: '%s', %lld.%lld | PID: '%d'", comp_name->value, tv.tv_sec, tv.tv_usec, component_module_pid, component_pid);
  tracef("{'type':'dispatch_set', 'component':'%s','PID':%d}",comp_name->value, component_module_pid);
  msg_Info( gl_p_intf, "\tCSIName: %s \n HAState: %s \n CSIFlags: %s ",
    csi_desc.csiName.value, ha_state_str[ha_state], csi_flag_str[csi_desc.csiFlags]);
  tracef("{'type':'csi_assignment', 'CSI':'%s', 'component':'%s' , 'HAState':'%s' , 'CSIFlags':'%s', 'PID':%d}",
    csi_desc.csiName.value, comp_name->value ,ha_state_str[ha_state], csi_flag_str[csi_desc.csiFlags], component_module_pid);
  // Get the CSI we're acting uppon
  if ( csi_desc.csiFlags != SA_AMF_CSI_TARGET_ALL &&
       ( csi_desc.csiName.length != p_sys->amf_csi_name.length
       || strncmp((const char *)csi_desc.csiName.value, (const char *) p_sys->amf_csi_name.value,csi_desc.csiName.length) != 0 ) ) {
      if ( SA_AMF_HA_QUIESCED == ha_state || SA_AMF_HA_QUIESCING == ha_state ) {
        // Don't know how to quiesc a CSI we've never heard about
        rc = saAmfResponse(p_sys->amf_hdl, inv, SA_AIS_ERR_FAILED_OPERATION);
      } else
        p_sys->amf_csi_name = csi_desc.csiName;
  }
  
  // Transition HA state
  switch (ha_state) {
    case SA_AMF_HA_ACTIVE:
      // Active
          msg_Info( gl_p_intf, "Doing active");
          tracef("Doing active");
      if (!p_sys->checkpoint)
    saCkptCheckpointClose(p_sys->checkpoint);
          msg_Info( gl_p_intf, "after ckpt close");
          tracef("after ckpt close");
      rc = saCkptCheckpointOpen(p_sys->ckpt_hdl, &ckpt_name,
              &ckpt_attrs,
              SA_CKPT_CHECKPOINT_READ | SA_CKPT_CHECKPOINT_WRITE | SA_CKPT_CHECKPOINT_CREATE,
              SA_TIME_ONE_MINUTE, &p_sys->checkpoint);
      if ( SA_AIS_OK != rc ) {
        msg_Err( gl_p_intf, "saCkptCheckpointOpen FAILED - %u", rc);
        tracef("saCkptCheckpointOpen failed; handle returned: %u", rc);
        saAmfResponse(p_sys->amf_hdl, inv, rc);
        return;
      }
          msg_Info( gl_p_intf, "Checkpoint open");
          tracef("Checkpoint open");
      rc = saCkptActiveReplicaSet(p_sys->checkpoint);
      if ( SA_AIS_OK != rc ) {
        msg_Err( gl_p_intf, "saCkptActiveReplicaSet FAILED - %u", rc);
        tracef("saCkptActiveReplicaSet failed; handle returned: %u", rc);
        saAmfResponse(p_sys->amf_hdl, inv, rc);
        return;
      }
          msg_Info( gl_p_intf, "Active Replica Set.");
      p_sys->is_active = true;
      
      // Start service
      // If there's no active checkpoint, load default config and we're done.
      // Checkpoint exists. load configuration
#if 0
      SaCkptSectionIterationHandleT handle;
      SaCkptSectionDescriptorT  sectionDescriptor;

      rc = saCkptSectionIterationInitialize(p_sys->checkpoint, SA_CKPT_SECTIONS_ANY, 0, &handle);
      if ( SA_AIS_OK != rc ) {
        msg_Err( gl_p_intf, "saCkptSectionIterationInitialize FAILED - %u", rc);
    saAmfResponse(p_sys->amf_hdl, inv, rc);
    return;
      }
          msg_Info( gl_p_intf, "Got itterator");
      rc = saCkptSectionIterationNext(handle, &sectionDescriptor);
      // if ( SA_AIS_ERR_NO_SECTIONS == rc ) {
      // } else
      if ( SA_AIS_OK != rc ) {
        msg_Err( gl_p_intf, "saCkptSectionIterationNext FAILED - %u", rc);
    saAmfResponse(p_sys->amf_hdl, inv, rc);
    return;
      }
          msg_Info( gl_p_intf, "Got next");
      saCkptSectionIterationFinalize(handle);
          msg_Info( gl_p_intf, "Finalized");
#endif
  
      bool load_defaults = true;
      char * vlm_config;

      vector.sectionId = playlist_section;
      vector.dataBuffer = &section_length;
      vector.dataSize = sizeof(section_length);
      vector.dataOffset = 0;

      rc = saCkptCheckpointRead(p_sys->checkpoint, &vector, 1, &erroneousVectorIndex);
      if ( SA_AIS_OK == rc && section_length > 0 ) {
        vlm_config = malloc(section_length+1);
    if ( !vlm_config ) {
        msg_Err( gl_p_intf, "malloc for config of size %llu failed", (long long unsigned int) section_length);
        saAmfResponse(p_sys->amf_hdl, inv, SA_AIS_ERR_NO_MEMORY);
        return;
    }

    vector.dataBuffer = vlm_config;
    vector.dataSize = section_length;
    vector.dataOffset = sizeof(section_length);
    rc = saCkptCheckpointRead(p_sys->checkpoint, &vector, 1, &erroneousVectorIndex);
    if ( SA_AIS_OK == rc )
        load_defaults = false;
    vlm_config[section_length] = 0;

            msg_Info( gl_p_intf, "Found old configuration: %s", vlm_config);
      }
      if (load_defaults) {
        if (csi_desc.csiFlags == SA_AMF_CSI_TARGET_ALL) {
        msg_Err( gl_p_intf, "Internal error: can't load defualt config when assigned with SA_AMF_CSI_TARGET_ALL" );
        saAmfResponse(p_sys->amf_hdl, inv, SA_AIS_ERR_TRY_AGAIN);
        return;
    }
    typeof(csi_desc.csiAttr.number) i;
    for (i = 0; i < csi_desc.csiAttr.number ; ++i)
        if (! strcmp((const char *)csi_desc.csiAttr.attr[i].attrName, "config") )
          break;
    if (i < csi_desc.csiAttr.number ) {
        struct stat buf;
        const char * const file_name = (char *) csi_desc.csiAttr.attr[i].attrValue;
        if ( stat(file_name, &buf) ) {
          msg_Info( gl_p_intf, "Unable to stat configuration file \"%s\"", file_name );
      saAmfResponse(p_sys->amf_hdl, inv, SA_AIS_ERR_INVALID_PARAM);
      return;
        }
        if (! (vlm_config = malloc(buf.st_size + 1)) ) {
          msg_Err( gl_p_intf, "malloc for config of size %llu failed while loading trying to load '%s'", (unsigned long long) buf.st_size, file_name);
      saAmfResponse(p_sys->amf_hdl, inv, SA_AIS_ERR_NO_MEMORY);
      return;
        }
        int fd;
        if (! (fd = open(file_name, O_RDONLY|O_LARGEFILE)) ) {
          msg_Err( gl_p_intf, "Unable to open onfiguration file '%s' - 0x%X, - %s", file_name, errno, strerror(errno) );
      free(vlm_config);
      saAmfResponse(p_sys->amf_hdl, inv, SA_AIS_ERR_INVALID_PARAM);
      return;
        }
        ssize_t size;
        if ( (size = read(fd, vlm_config, buf.st_size)) < 0 ) {
          msg_Err( gl_p_intf, "Error reading default configuration file '%s' - 0x%X, - %s", file_name, errno, strerror(errno) );
      free(vlm_config);
      saAmfResponse(p_sys->amf_hdl, inv, SA_AIS_ERR_INVALID_PARAM);
      return;
        }
        vlm_config[size] = 0;

        msg_Info( gl_p_intf, "Using default configuration file");
    } else {
        msg_Err( gl_p_intf, "No default configuration specified!" );
        saAmfResponse(p_sys->amf_hdl, inv, SA_AIS_ERR_INVALID_PARAM);
        return;
    }

    //defaults loaded, make checkpoint sections 
    SaCkptSectionCreationAttributesT screate;
    screate.sectionId = &playlist_section;
    screate.expirationTime = SA_TIME_END;
    section_length = 0;
    rc = saCkptSectionCreate(p_sys->checkpoint, &screate, &section_length, sizeof(section_length));
    if (SA_AIS_OK == rc || SA_AIS_ERR_EXIST == rc) {
        screate.sectionId = &clients_section;
        rc = saCkptSectionCreate(p_sys->checkpoint, &screate, &section_length, sizeof(section_length));
    }
    if (SA_AIS_OK == rc || SA_AIS_ERR_EXIST == rc) {
        screate.sectionId = &positions_section;
        rc = saCkptSectionCreate(p_sys->checkpoint, &screate, &section_length, sizeof(section_length));
    }
    if (SA_AIS_OK != rc && SA_AIS_ERR_EXIST != rc) {
        msg_Err( gl_p_intf, "saCkptSectionCreate FAILED for %s - %u", screate.sectionId->id, rc);
        free(vlm_config);
        saAmfResponse(p_sys->amf_hdl, inv, rc);
        return;
    }
    p_sys->need_pl_checkpoint = 1;
      }
          msg_Info( gl_p_intf, "loading config");

      char *cmd_start = vlm_config, *cmd_end;
      while ( 1 ) {
        bool need_break = false;
        cmd_end = cmd_start;
    while (*cmd_end != 0 && *cmd_end != '\n')
        ++cmd_end;
    if (*cmd_end == '\n')
        *cmd_end = 0;
    else if (*cmd_end == 0)
        need_break = true;
              msg_Info( gl_p_intf, "loading: %s", cmd_start);
    vlm_ExecuteCommand( p_sys->mediatheque, cmd_start, &message );
              msg_Info( gl_p_intf, "loaded");
    if (message) {
        char * output = MessageToString(message,0);
        if (strlen(output) > 4)
          msg_Err( gl_p_intf, "Load: %s", output);
        free(output);
        vlm_MessageDelete( message );
        message = NULL;
    }
    if (need_break)
        break;
    else
        cmd_start = cmd_end + 1;
              msg_Info( gl_p_intf, "Next");
      }
          msg_Info( gl_p_intf, "loaded config");
      free(vlm_config);

      //vlm_ExecuteCommand( p_sys->mediatheque, "control aa play\n", &message );
      //vlm_MessageDelete( message );
          //msg_Info( gl_p_intf, "playing");

      // Load user possition
      vector.sectionId =  positions_section;
      vector.dataBuffer = &pos;
      vector.dataSize = sizeof(pos);
      vector.dataOffset = 0;

      rc = saCkptCheckpointRead(p_sys->checkpoint, &vector, 1, &erroneousVectorIndex);
      if ( SA_AIS_OK == rc ) {
            msg_Info( gl_p_intf, "doing play_at, %lld", pos );
        sprintf(seek_str, "control aa play_at %lld\n", pos);
      } else {
            msg_Info( gl_p_intf, "doing play" );
        sprintf(seek_str, "control aa play\n", pos);
      }
            vlm_ExecuteCommand( p_sys->mediatheque, seek_str, &message );
    if (message) {
        char * output = MessageToString(message,0);
        if (strlen(output) > 4)
          msg_Err( gl_p_intf, "Seek: %s", output);
        free(output);
        vlm_MessageDelete( message );
        message = NULL;
    }
      msg_Info( gl_p_intf, "after pos");

      rc = saAmfResponse(p_sys->amf_hdl, inv, SA_AIS_OK);
      gettimeofday(&tv, NULL);
          msg_Info( gl_p_intf, "active done %lld.%lld", tv.tv_sec, tv.tv_usec);
      sleep(3);
      break;
    case SA_AMF_HA_STANDBY:
      // Standby: do nothing except open a local copy of the checkpoint
          msg_Info( gl_p_intf, "Doing standby");
      p_sys->is_active = false;
      if (p_sys->checkpoint) {
            msg_Info( gl_p_intf, "doing close");
    saCkptCheckpointClose(p_sys->checkpoint);
    p_sys->checkpoint = 0;
      }
          msg_Info( gl_p_intf, "after close");
      saCkptCheckpointOpen(p_sys->ckpt_hdl, &ckpt_name,
         NULL,
         SA_CKPT_CHECKPOINT_READ,
         SA_TIME_ONE_MINUTE, &p_sys->checkpoint);
      // Ignore errors
      gettimeofday(&tv, NULL);
          msg_Info( gl_p_intf, "open done %lld.%lld", tv.tv_sec, tv.tv_usec);
      rc = saAmfResponse(p_sys->amf_hdl, inv, SA_AIS_OK);
      break;
    case SA_AMF_HA_QUIESCED:
      // Un-clean termination of service state for switchover
      // save checkpoint and exit
      perform_checkpoint(gl_p_intf);
    case SA_AMF_HA_QUIESCING:
      p_sys->is_active = false;
      rc = saAmfResponse(p_sys->amf_hdl, inv, SA_AIS_OK);
      // Clean termination of service
      if (p_sys->checkpoint) {
    saCkptCheckpointClose(p_sys->checkpoint);
    p_sys->checkpoint = 0;
      }
      libvlc_Quit( gl_p_intf->p_libvlc );
      if (SA_AMF_HA_QUIESCING == ha_state) {
    SaAisErrorT rc = saAmfCSIQuiescingComplete(p_sys->amf_hdl, inv, SA_AIS_OK);
    if ( SA_AIS_OK != rc )
        msg_Err( gl_p_intf, "saAmfCSIQuiescingComplete FAILED - %u", rc);
      }else{
        tracef("saAmfCSIQuiescingComplete successful");
      }
      break;
  }
  
  if ( SA_AIS_OK != rc ) {
    msg_Err( gl_p_intf, "saAmfResponse FAILED - %u", rc);
  }
}

/****************************************************************************
  Name          : avsv_amf_csi_remove_callback
 
  Description   : This routine is a callback to remove the CSI (or all the 
      CSIs) that is/are assigned to the component. It is specified
      as a part of AMF initialization.
 
  Arguments     : inv       - particular invocation of this callback function
      comp_name - ptr to the component name
      csi_name  - ptr to the CSI name that is being removed
      csi_flags - specifies if one or more CSIs are affected

  Return Values : None.
 
  Notes         : None
******************************************************************************/
void vlc_amf_csi_remove_callback(SaInvocationT  inv, 
          const SaNameT  *comp_name,
          const SaNameT  *csi_name,
          SaAmfCSIFlagsT csi_flags)
{
  intf_sys_t    *p_sys  = gl_p_intf->p_sys;
  SaAisErrorT rc;

  struct timeval tv;
  gettimeofday(&tv, NULL);
  int component_module_pid = getpid();
  msg_Info( gl_p_intf, "\n Dispatched 'CSI Remove' Callback \n Component: %s \n CSI: %s \n CSIFlags: %s %lld.%lld", 
         comp_name->value, csi_name->value, csi_flag_str[csi_flags], tv.tv_sec, tv.tv_usec);
  tracef("{'type':'dispatch_remove' , 'component': '%s' , 'CSI':'%s' , 'CSIFlags': '%s' , 'PID': '%d'}", 
         comp_name->value, csi_name->value, csi_flag_str[csi_flags], component_module_pid);

  // Get the CSI we're acting uppon
  if ( csi_flags != SA_AMF_CSI_TARGET_ALL &&
       ( csi_name->length != p_sys->amf_csi_name.length ||
       strncmp((const char *) csi_name->value , (const char *)p_sys->amf_csi_name.value, csi_name->length) != 0 ) ) {
      // We don't know about it
      saAmfResponse(p_sys->amf_hdl, inv, SA_AIS_OK);
      return;
  }

  if (p_sys->checkpoint) {
      saCkptCheckpointClose(p_sys->checkpoint);
      p_sys->checkpoint = 0;
  }
        
  p_sys->amf_csi_name.length = 0;
  
  rc = saAmfResponse(p_sys->amf_hdl, inv, SA_AIS_OK);
  if ( SA_AIS_OK != rc ) {
    msg_Err( gl_p_intf, "saAmfResponse FAILED - %u", rc);
    tracef("saAmfResponse FAILED - %u", rc);
  }
}

/****************************************************************************
  Name          : avsv_amf_comp_terminate_callback
 
  Description   : This routine is a callback to terminate the component. It 
      is specified as a part of AMF initialization.
 
  Arguments     : inv             - particular invocation of this callback 
            function
      comp_name       - ptr to the component name
 
  Return Values : None.
 
  Notes         : None
******************************************************************************/
void vlc_amf_comp_terminate_callback(SaInvocationT inv, 
              const SaNameT *comp_name)
{
  intf_sys_t    *p_sys  = gl_p_intf->p_sys;
  SaAisErrorT rc;

  struct timeval tv;
  gettimeofday(&tv, NULL);
  int component_module_pid = getpid();
  msg_Info( gl_p_intf, "Dispatched 'Component Terminate' Callback \n Component: %s %lld.%lld", 
         comp_name->value, tv.tv_sec, tv.tv_usec);
  tracef("{ 'type':'dispatch_terminate' ,'component': '%s', 'PID': '%d'}", 
         comp_name->value, component_module_pid);

  libvlc_Quit( gl_p_intf->p_libvlc );
         
  /* Respond immediately */
  rc = saAmfResponse(p_sys->amf_hdl, inv, SA_AIS_OK);
  if ( SA_AIS_OK != rc )
    msg_Err( gl_p_intf, "saAmfResponse FAILED - %u", rc);
}


/* We need the level of the message to put a beautiful indentation.
 * first level is 0 */
static char *MessageToString( vlm_message_t *message, int i_level )
{
#define STRING_CR "\r\n"
#define STRING_TAIL "> "

    char *psz_message;
    int i, i_message = sizeof( STRING_TAIL );

    if( !message || !message->psz_name )
    {
        return strdup( STRING_CR STRING_TAIL );
    }
    else if( !i_level && !message->i_child && !message->psz_value  )
    {
        /* A command is successful. Don't write anything */
        return strdup( /*STRING_CR*/ STRING_TAIL );
    }

    i_message += strlen( message->psz_name ) + i_level * sizeof( "    " ) + 1;
    psz_message = xmalloc( i_message );
    *psz_message = 0;
    for( i = 0; i < i_level; i++ ) strcat( psz_message, "    " );
    strcat( psz_message, message->psz_name );

    if( message->psz_value )
    {
        i_message += sizeof( " : " ) + strlen( message->psz_value ) +
            sizeof( STRING_CR );
        psz_message = xrealloc( psz_message, i_message );
        strcat( psz_message, " : " );
        strcat( psz_message, message->psz_value );
        strcat( psz_message, STRING_CR );
    }
    else
    {
        i_message += sizeof( STRING_CR );
        psz_message = xrealloc( psz_message, i_message );
        strcat( psz_message, STRING_CR );
    }

    for( i = 0; i < message->i_child; i++ )
    {
        char *child_message =
            MessageToString( message->child[i], i_level + 1 );

        i_message += strlen( child_message );
        psz_message = xrealloc( psz_message, i_message );
        strcat( psz_message, child_message );
        free( child_message );
    }

    if( i_level == 0 ) strcat( psz_message, STRING_TAIL );

    return psz_message;
}
