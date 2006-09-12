<?php

#
# Copyright (c) 2006 Sun Microsystems, Inc.
#                         All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

#
#
# Print a report in tabular format
#
#

$topdir = ".";
include_once("$topdir/reporter.inc");
include_once("$topdir/database.inc");
include_once("$topdir/http.inc");
include_once("$topdir/config.inc");
include_once("$topdir/html.inc");
include_once("$topdir/help.inc");

$groupbys_str = "";
$orderbys_str = "";
$selects_str  = "";

$selects                         = array();
$selects['per_script']['params'] = array();
$selects['per_level']['all']     = array();
$selects['per_level']['details'] = array();
$selects['per_level']['params']  = array();
$selects['per_level']['results'] = array();
$selects['per_phase']['all']     = array();

$filters = array();

# Dump query description, results table, and link to query
function dump_report() {

    # global declarations
    global $dbname, $user, $pass;
    global $domain;
    global $phases, $phase_labels;
    global $selects, $filters;
    global $results_types;
    global $fields_run_key;
    global $tables;
    global $config, $level;
    global $help;
    global $field_labels, $All;
    global $groupbys_str, $orderbys_str, $selects_str;
    global $cgi_abbrevs;
    global $timestamp;
    global $thcolor, $gray, $dgray, $lgray, $llgray, 
           $lllgray, $lred, $lgreen, $lyellow, $white;

    if (! ($conn = pg_connect("host=localhost port=5432 dbname=$dbname user=$user password=$pass")))
        exit_("<br><b><i>Could not connect to database server.</i></b>");

    debug("\n<br>postgres: " . pg_last_error() . "\n" . pg_result_error());

    # In the query tool, there is just a single '$level' of detail per
    # invocation of the script

    if ($_GET['cgi'] == 'on')
        debug_cgi($_GET, "GET");

    # --- Hack CGI params

    $phases['per_level'] = get_phase_selection($_GET);

    $filters = get_date_filter($_GET[$cgi_abbrevs['mainfield'] . "$timestamp"]);
    $filters = array_merge($filters, get_menu_filters($_GET));
    $filters = array_merge($filters, get_textfield_filters($_GET));
    $filters = array_merge($filters, get_results_filter($_GET));

    $selects['per_script']['params'] = get_selects();

    # Split out selects into params and results
    $selects['per_level']['params'] =
        array_merge(
            (isset($config['by_run'][$level]) ?
                array_diff($selects['per_script']['params'], $fields_run_key) :
                $selects['per_script']['params'])
        );

    var_dump_html("[dump_report " . __LINE__ . "] selects:", $selects);

    # We always need to get the first table by case, before aggregating
    # runs from that result set
    $selects['per_level']['results'] =
        get_phase_result_selects($phases['per_level'], 'by_case');

    $unioned_queries = array();

    # Compose phase-specific queries and union them for each level

    foreach ($phases['per_level'] as $phase) {
        array_push($unioned_queries, create_phase_level_query($phase));
    }

    # Concat the results from the three phases
    $cmd = join("\n UNION ALL \n", $unioned_queries);

    $cmd = "\nSELECT * INTO TEMPORARY TABLE tmp FROM (" . $cmd . ") as u;";

    debug_sql($cmd);

    pg_query_("\n$cmd", $conn);

    # Unfortunately, we need to split out 'params', 'results', and 'details'
    # fields so we can create headers and linked data correctly
    $selects['per_level']['params'] =
        array_map('get_as_alias',
            array_merge(
                $selects['per_level']['params']
                # (($config['add_params'][$level] and (sizeof($phases['per_level']) < 3)) ?
                #     $config['add_params'][$level][$phases['per_level'][0]] : # blech!
                #     null)
            )
        );
    $selects['per_level']['details'] =
        array_map('get_as_alias',
            array_merge(

                # Give good reason to add that far right link!
                (($config['details'][$level][$phase] and
                  ! isset($config['by_run'][$level]) and
                  ! isset($_GET['no_details']) and
                 (sizeof($phases['per_level']) == 1)) ?
                    $config['details'][$level][$phase] :
                     null)
            )
        );
    $selects['per_level']['all'] =
        array_merge(
            $selects['per_level']['params'],
            $selects['per_level']['results'],
            $selects['per_level']['details']
        );

    # Select from the unioned tables which is now named 'tmp'
    $cmd = "\nSELECT " .
            join(",\n", array_unique($selects['per_level']['all'])) .  " " .
            "\n\tFROM tmp ";
    if ($groupbys_str)
        $cmd .= "\n\tGROUP BY $groupbys_str ";
    if ($orderbys_str)
        $cmd .= "\n\tORDER BY $orderbys_str ";

    $sub_query_alias = 'run_atomic';

    if (isset($config['by_run'][$level])) {

        $selects['per_script']['params'] =
            get_non_run_key_params($selects['per_script']['params']);

        $cmd = "\nSELECT " .
                join(",\n",
                    array_unique(
                        array_merge(
                            $selects['per_level']['params'],
                            get_phase_result_selects($phases['per_level'], 'by_run')
                        )
                    )
                ) .
                "\nFROM ($cmd) as $sub_query_alias " .
                "\nGROUP BY $sub_query_alias.". join(",\n$sub_query_alias.",$selects['per_level']['params']);
    }

    debug_sql($cmd);

    $rows = pg_query_("\n$cmd", $conn);

    # Create a new temp table for every level
    $cmd = "\nDROP TABLE tmp;";
    pg_query_("\n$cmd", $conn);

    # --- Generate headers

    # Param headers
    $headers['params'] = array();
    foreach (array_map('get_as_alias', $selects['per_level']['params']) as $key) {
        $header = $field_labels[$key];
        array_push($headers['params'], $header);
    }

    # This block should be useful for a customized report (e.g., summary.php)
    if (isset($_GET['just_results'])) {
        dump_data_table($headers, $rows);
        return;
    }

    $filters_desc_html_table = filters_description($filters);

    # Report description (echo user input)
    if ($filters_desc_html_table and ! isset($_GET['just_results'])) {
        $filters_desc_html_table = "<a name=report></a>" .
                                   "<br><table width=100%><tr>" .
                                   "<td valign=top>$filters_desc_html_table" .
                                   "<td valign=top align='right'><a href='$domain'>" .
                                   "<img src='$topdir/images/open-mpi-logo.png' border=0 height=75 " .
                                   "alt='$domain/mtt'></a>" .
                                   "</table><br>";
    } else
        $filters_desc_html_table = null;


    print $filters_desc_html_table;

    dump_data_table($headers, $rows);

    print info_pointers();
    print help(phases_help($phases));

    pg_close();
}

# void function to print out html data table
function dump_data_table($headers, $rows) {

    # global declarations
    global $phases, $phase_labels;
    global $selects;
    global $field_labels;
    global $results_types;
    global $translate_data_cell;
    global $thcolor, $gray, $dgray, $lgray, $llgray, 
           $lllgray, $lred, $lgreen, $lyellow, $white;

    # Do not print an empty table
    if (! $rows) {
        print "<b><i>No data available for the specified query.</i></b>";
        return;
    }
    
    # Insert useful information on the left-hand side?
    print "\n\n<div align=center>" .
          "\n\n<!-- report_start -->\n\n" .
          "<table width='100%' cellpadding=5>" .
          "<tr>" .
          #"<th bgcolor='$lgray' rowspan=2 colspan=2 valign='top' width=0px>[insert link here]" .
          "";

    print "\n<td bgcolor='$lgray'>";

    print "\n\n<div align=center><table border=1 width='100%'>";

    # Display headers
    print 
        sprintf_("\n<th bgcolor='$thcolor' rowspan=2>%s", $headers['params']);

    $i = 1;
    foreach ($phases['per_level'] as $ph) {
        print 
            sprintf("\n<th bgcolor='$thcolor' colspan=2>%s", 
                $phase_labels[$ph] . help("<sup>[" . $i++ . "]"));
    }
    if ($selects['per_level']['details'])
        print sprintf("\n<th bgcolor='$thcolor' rowspan=2>%s", "[i]");

    print "\n<tr>";

    # Yucky hard-coding, but will it ever be anything but pass/fail here?
    foreach ($phases['per_level'] as $p) {
        print sprintf("\n<th bgcolor='$thcolor'>%s", 'Pass');
        print sprintf("\n<th bgcolor='$thcolor'>%s", 'Fail');
    }

    # Display data rows
    while ($row = array_shift($rows)) {

        $details_html_table = "";

        # Make the row clickable if there's clickable info for this query
        if ($selects['per_level']['details']) {

            $len = sizeof($selects['per_level']['details']);
            $linked_field_cols = array_splice($row, sizeof($row) - $len, $len);

            $details_html_table = "\n\n" .
                "<table border=1 width=100%><tr><th bgcolor=$thcolor>Details" .
                "<tr><td bgcolor=$lllgray width=100%>";

            for ($i = 0; $i < $len; $i++) {
                $field = $selects['per_level']['details'][$i];
                $field = $field_labels[$field] ? $field_labels[$field] : $field;
                $details_html_table .= "\n<br><b>" .
                        $field . "</b>:<br>" .
                        "<tt>" . txt_to_html($linked_field_cols[$i]) . "</tt><br>";
            }
            $details_html_table .= "</table></body>";
        }

        # translate_data_cell result fields
        for ($i = 0; $i < sizeof($row); $i++) {
            $row[$i] = 
                (! empty($translate_data_cell[$row[$i]])) ? $translate_data_cell[$row[$i]] : $row[$i];
        }

        # 'pass/fail' are always in the far right cols
        $len = sizeof($phases['per_level']) * sizeof($results_types);
        $result_cols = array_splice($row, sizeof($row) - $len, $len);

        print "\n<tr>" . sprintf_("\n<td bgcolor=$white>%s", $row);

        for ($i = 0; $i < sizeof($result_cols); $i += 2) {
            print "\n<td align='right' bgcolor='" .
                    (($result_cols[$i] > 0) ? $lgreen : $lgray) . "'>$result_cols[$i]";
            print "\n<td align='right' bgcolor='" .
                    (($result_cols[$i + 1] > 0) ? $lred : $lgray) . "'>" . $result_cols[$i + 1];
        }

        if ($details_html_table) {

            print "<td align=center bgcolor=$lgray>" .
                  "<a href='javascript:popup(\"900\",\"750\",\"" .
                  "$phase_labels[$phase]: Detailed Info\",\"" .
                  strip_quotes($details_html_table) . "\",\"\",\" font-family:Courier,monospace\")' " .
                     " class='lgray_ln'><font size='-2'>" .
                  "[i]</font><a>";
        }
    }

    print "\n</table>";
    print "\n</table>";
    print "\n\n<!-- report_end -->\n\n";
}

# Return an HTML table describing the SQL filters used in $filters
function filters_description($filters) {

    global $config, $level;
    global $thcolor, $gray, $dgray, $lgray, $llgray, $lllgray, $lred, $lgreen, $lyellow, $white;
    global $help;

    # Create a plain-english description of the filters
    if (sizeof($filters) > 0) {
        $filters_desc_html_table = "<table border=1 width=60%><tr>" .
            "<th bgcolor=$lgray colspan=2>Query Description";

        foreach ($filters as $filter) {
            $tuple = sql_to_en($filter);
            $filters_desc_html_table .=
                sprintf("\n<tr><td bgcolor=$lgray>%s<td bgcolor=$llgray>%s",
                    ascii_sp_to_html_sp($tuple[0]), 
                    ascii_sp_to_html_sp($tuple[1]));
        }
        $count = (isset($config['by_run'][$level]) ? "By test run" : "By test case");
        $filters_desc_html_table .= 
            # The 'Count' setting is not used at the $filters level
            "<tr><td bgcolor=$lgray>Count <td bgcolor=$llgray>" .
            $count . " *" .
            "<tr><td colspan=2 bgcolor=$lllgray><i><font size=-2>*" . 
                $help[$count] . "</font></i>" .
            "</table><br>" .
            "";
    }
    else
        $filters_desc_html_table = null;

    return $filters_desc_html_table;
}

# Return an array of all that's to be included in SELECT
function get_selects() {
    
    global $phases;
    global $config, $level;
    global $cgi_abbrevs;
    global $agg_timestamp_selects;

    $cgi_selects = array();

    # agg_timestamp is an oddball agg_ in that it creates a select
    $agg_timestamp = $_GET[$cgi_abbrevs['mainfield'] . 'agg_timestamp'];
    if ($agg_timestamp != "-")
        $cgi_selects = array($agg_timestamp_selects[$agg_timestamp]);

    $cgi_selects = array_merge($cgi_selects, get_select_fields($_GET));

    # Add additional information if they select only a single phase

    if (sizeof($phases['per_level']) == 1)
        $cgi_selects =
            array_merge($cgi_selects, $config['add_params'][$level][$phases['per_level'][0]]);

    # Show less when they checkbox "aggregate"
    $cgi_selects = array_filter($cgi_selects, "is_not_rolled");

    # Use array_unique as a safeguard against SELECT-ing duplicate fields
    return array_unique($cgi_selects);
}

# Create a subquery at the phase level for combining into a single table
function create_phase_level_query($phase) {

    global $filters;
    global $selects;
    global $results;
    global $config, $level;
    global $fields_run_key;
    global $phases;
    global $once_db_table;
    global $groupbys_str, $orderbys_str, $selects_str;

    # db table names are identical to phase names used in this script
    $db_table = $phase;

    # Create a tmp list of select fields to copy from and manipulate
    $selects['per_phase']['all'] = array();
    $selects['per_phase']['all'] =
        array_merge(
            (isset($config['by_run'][$level]) ?
                array_diff($selects['per_level']['params'], $fields_run_key) :
                $selects['per_level']['params']),
            (isset($config['by_run'][$level]) ?
                $fields_run_key :
                null),
            # (($config['add_params'][$level][$phase] and
            #  (sizeof($phases['per_level']) < 3)) ?
            #     $config['add_params'][$level][$phase] :
            #      null),

             $results['from_perm_tbl'][$phase],

            # Give good reason to add that far right link!
            (($config['details'][$level][$phase] and
              ! isset($config['by_run'][$level]) and
              ! isset($_GET['no_details']) and
             (sizeof($phases['per_level']) == 1)) ?
                $config['details'][$level][$phase] :
                 null)
        );

    # Assemble GROUP BY and ORDER BY clauses.
    # If we do an SQL string function, trim it to just the arg (groupbys and
    # orderbys are the same lists as selects, without the string functions and
    # result fields)
    $groupbys = array();
    $orderbys = array();

    # [ ] Use a combo of array_map and array_filter here
    foreach (array_unique($selects['per_phase']['all']) as $s) {

        # Do not group or sort on these two aggregates
        if (preg_match("/test_pass|success/i", $s))
            continue;

        $s = get_as_alias($s);
        array_push($groupbys, $s);
        array_push($orderbys, $s);
    }

    $groupbys_str = join(",\n", $groupbys);
    $orderbys_str = join(",\n", $orderbys);
    $selects_str = join(",\n", $selects['per_phase']['all']);

    # Compose SQL query
    $cmd = "\nSELECT $selects_str \nFROM $db_table JOIN $once_db_table USING (run_index) ";
    $cmd .= ((sizeof($filters) > 0) ?  "\nWHERE " . join("\n AND \n", $filters) : "") .
            " ";

    return $cmd;
}

# Use implicit selection, as well as process of elimination to determine which
# phases should be shown
function get_phase_selection($params) {

    global $All;
    global $phases;
    global $cgi_abbrevs;

    $phase_selection = array();
    $selected        = $params[$cgi_abbrevs['mainfield'] . 'phase'];
    $all_phases      = $phases['per_script'];
    $which_phases    = which_phase_specific_filter($params);

    # Did they search on a phase specific field?
    if ($which_phases != null) {
        $phase_selection = $which_phases;
    }
    # Did they specify a specific phase in the phase menu?
    elseif ($selected != $All) {
        $phase_selection = array($selected);
    }
    # Default: show all phases
    else {
        $phase_selection = $all_phases;
    }
    return $phase_selection;
}

# Show links to point user to more info ...
function info_pointers() {

    global $thcolor, $gray, $dgray, $lgray, $llgray, 
           $lllgray, $lred, $lgreen, $lyellow, $white;
    global $domain;

    $links = "\n<div align=left><br><br><table border=1><tr><td bgcolor=$lgray><a href='" . $domain .
                $_SERVER['PHP_SELF'] . '?' .
                dump_query_string($_GET) .
                "' class='lgray_ln'><font size='-1'>[Link to this query]</a>" .
                "</table>";

    if (isset($_GET['info_links'])) {

        $self    = $_SERVER['PHP_SELF'];
        $customs = preg_replace('/reporter.php.*$/', 'reporter.php', $self);
        $summary = preg_replace('/reporter.php.*$/', 'summary.php', $self);

        $links .= "\n<div align=left><br>" .
                   "\n<table border=1><tr><td bgcolor=$lgray><a href='" . 
                   # Note: do *not* put a slash between domain and path
                   $domain  . $summary . "' class='lgray_ln'><font size='-1'>[Summary report]</a>" .
                   "</table>";

        $links .= "\n<div align=left><br>" .
                   "\n<table border=1><tr><td bgcolor=$lgray><a href='" . 
                   $domain . $customs . "' class='lgray_ln'><font size='-1'>[Custom reports]</a>" .
                   "</table>";
    }
    return $links;
}

# Return HTML string displaying explanation of phases
function phases_help($phases) {

    global $help;

    $i = 1;
    $phases_help = "\n<div align=left><br>" .
                   "\n<table border=0 width=40%><tr><td>" .
                   "\n<br><i><font size=-2>" . $help['Phase'] . "</font></i><br>";

    foreach ($phases['per_level'] as $ph) {
        $phases_help .= 
            "\n<br><i><font size=-2><strong><sup>[" . $i++ . "]</strong> " . $help[$ph] . "</font></i>";
    }
    $phases_help .= "\n</table>";

    return $phases_help;
}

?>