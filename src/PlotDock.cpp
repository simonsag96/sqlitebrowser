#define QT_NO_FLOAT16_OPERATORS     // This works around https://bugreports.qt.io/browse/QTBUG-72073 which makes the build fail on MSVC 2017 and Qt 5.12

#include "PlotDock.h"
#include "ui_PlotDock.h"
#include "Settings.h"
#include "sqlitetablemodel.h"
#include "FileDialog.h"
#include "TableBrowser.h"     // Just for BrowseDataTableSettings, not for the actual table browser class

#include <QPrinter>
#include <QPrintPreviewDialog>
#include <QDesktopServices>
#include <QUrl>
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#include <QRandomGenerator>
#endif

// Enable this line to show the most basic performance stats after pressing the fetch-all-data button. Please keep in mind that while these
// numbers might help to estimate the performance of the data loading procedure, this is not a proper benchmark.
//#define LOAD_DATA_BENCHMARK

#ifdef LOAD_DATA_BENCHMARK
#include <QElapsedTimer>
#endif

#include <QMouseEvent>
#include <QLocale>

#include <algorithm>
#include <map>
#include <set>

static int random_number(int from, int to)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    return QRandomGenerator::global()->bounded(from, to);
#else
    return qrand() % to + from;
#endif
}

// The items of the comboColorGradient combo box are kept in the very same order as the
// QCPColorGradient::GradientPreset enum, so the combo index can be used as the preset directly.
static QCPColorGradient gradientFromIndex(int index)
{
    if(index < 0 || index > QCPColorGradient::gpHues)
        index = QCPColorGradient::gpThermal;

    QCPColorGradient gradient(static_cast<QCPColorGradient::GradientPreset>(index));
    // Points whose colour-column value is NULL are not part of the overlay (the series base colour
    // shows through for them), but make any stray NaN transparent just to be safe.
    gradient.setNanHandling(QCPColorGradient::nhTransparent);
    return gradient;
}

PlotDock::PlotDock(QWidget* parent)
    : QDialog(parent),
      ui(new Ui::PlotDock),
      m_currentPlotModel(nullptr),
      m_currentTableSettings(nullptr),
      m_showLegend(false),
      m_stackedBars(false),
      m_fixedFormat(false),
      m_xtype(QVariant::Invalid),
      m_colorScale(nullptr),
      m_colorScaleMarginGroup(nullptr)
{
    ui->setupUi(this);

    // Init widgets
    ui->treePlotColumns->setSelectionMode(QAbstractItemView::NoSelection);

    // Restore state
    ui->splitterForPlot->restoreState(Settings::getValue("PlotDock", "splitterSize").toByteArray());
    ui->comboLineType->setCurrentIndex(Settings::getValue("PlotDock", "lineType").toInt());
    ui->comboPointShape->setCurrentIndex(Settings::getValue("PlotDock", "pointShape").toInt());
    // Restore the gradient without emitting currentIndexChanged: the slot would call updatePlot()
    // before yAxes is initialised at the end of this constructor, which would crash.
    ui->comboColorGradient->blockSignals(true);
    ui->comboColorGradient->setCurrentIndex(Settings::getValue("PlotDock", "colorGradient").toInt());
    ui->comboColorGradient->blockSignals(false);

    // Connect signals
    connect(ui->plotWidget, &QCustomPlot::selectionChangedByUser, this, &PlotDock::selectionChanged);

    // connect slots that takes care that when an axis is selected, only that direction can be dragged and zoomed:
    connect(ui->plotWidget, &QCustomPlot::mousePress, this, &PlotDock::mousePress);
    connect(ui->plotWidget, &QCustomPlot::mouseWheel, this, &PlotDock::mouseWheel);
    connect(ui->plotWidget, &QCustomPlot::mouseMove, this, &PlotDock::mouseMove);

    // Enable: click on items to select them, Ctrl+Click for multi-selection, mouse-wheel for zooming and mouse drag for
    // changing the visible range.
    // Select one axis for zoom and drag applying only to that orientation.
    ui->plotWidget->setInteractions(QCP::iSelectPlottables | QCP::iMultiSelect | QCP::iRangeZoom | QCP::iRangeDrag | QCP::iSelectAxes);
    ui->plotWidget->setSelectionRectMode(QCP::srmNone);

    QShortcut* shortcutCopy = new QShortcut(QKeySequence::Copy, ui->plotWidget, nullptr, nullptr, Qt::WidgetShortcut);
    connect(shortcutCopy, &QShortcut::activated, this, &PlotDock::copy);

    QShortcut* shortcutPrint = new QShortcut(QKeySequence::Print, ui->plotWidget, nullptr, nullptr, Qt::WidgetShortcut);
    connect(shortcutPrint, &QShortcut::activated, this, &PlotDock::openPrintDialog);

    ui->plotWidget->setContextMenuPolicy(Qt::CustomContextMenu);

    // Set up context menu
    m_contextMenu = new QMenu(this);

    QAction* copyAction = new QAction(QIcon(":/icons/copy"), tr("Copy"), m_contextMenu);
    copyAction->setShortcut(shortcutCopy->key());
    m_contextMenu->addAction(copyAction);
    connect(copyAction, &QAction::triggered, this, [&]() {
       copy();
    });

    QAction* printAction = new QAction(QIcon(":/icons/print"), tr("Print..."), m_contextMenu);
    printAction->setShortcut(shortcutPrint->key());
    m_contextMenu->addAction(printAction);
    connect(printAction, &QAction::triggered, this, [&]() {
       openPrintDialog();
    });

    QAction* helpAction = new QAction(QIcon(":/icons/whatis"), tr("Help"), m_contextMenu);
    m_contextMenu->addAction(helpAction);
    connect(helpAction, &QAction::triggered, this, [&]() {
        QDesktopServices::openUrl(QUrl("https://github.com/sqlitebrowser/sqlitebrowser/wiki/Plot-Dock"));
    });

    QAction* showLegendAction = new QAction(tr("Show legend"), m_contextMenu);
    showLegendAction->setCheckable(true);
    m_contextMenu->addAction(showLegendAction);

    connect(showLegendAction, &QAction::toggled, this, &PlotDock::toggleLegendVisible);

    QAction* stackedBarsAction = new QAction(tr("Stacked bars"), m_contextMenu);
    stackedBarsAction->setCheckable(true);
    m_contextMenu->addAction(stackedBarsAction);

    connect(stackedBarsAction, &QAction::toggled, this, &PlotDock::toggleStackedBars);

    QAction* fixedFormatsAction = new QAction(tr("Fixed number format"), m_contextMenu);
    fixedFormatsAction->setCheckable(true);
    m_contextMenu->addAction(fixedFormatsAction);

    connect(fixedFormatsAction, &QAction::toggled, this,
      [=](bool fixed) {
          m_fixedFormat = fixed;
          adjustAxisFormat();
          ui->plotWidget->replot();
    });

    connect(ui->plotWidget, &QTableView::customContextMenuRequested, this,
            [=](const QPoint& pos) {
        // Show menu
        m_contextMenu->popup(ui->plotWidget->mapToGlobal(pos));
    });

    // Initialise the y axes and plotcolumn indices for y axes
    yAxes = {ui->plotWidget->yAxis, ui->plotWidget->yAxis2};
    PlotColumnY = {PlotColumnY1, PlotColumnY2};
}

PlotDock::~PlotDock()
{
    // Save state
    Settings::setValue("PlotDock", "splitterSize", ui->splitterForPlot->saveState());
    Settings::setValue("PlotDock", "lineType", ui->comboLineType->currentIndex());
    Settings::setValue("PlotDock", "pointShape", ui->comboPointShape->currentIndex());
    Settings::setValue("PlotDock", "colorGradient", ui->comboColorGradient->currentIndex());

    // Finally, delete all widgets
    delete ui;
}

void PlotDock::updatePlot(SqliteTableModel* model, BrowseDataTableSettings* settings, bool update, bool keepOrResetSelection)
{
    // Each column has an id that we use internally, starting from 0. However, at the beginning of the columns list we want to add
    // the virtual 'Row #' column which needs a separate unique id for internal use. This id is defined here as -1.
    const int RowNumId = -1;

    // add columns to x/y selection tree widget
    if(update)
    {
        // Store pointer to the current browse table settings in the main window
        m_currentTableSettings = settings;

        // disable tree plot columns item changed updates
        ui->treePlotColumns->blockSignals(true);

        m_currentPlotModel = model;

        // save current selected columns, so we can restore them after the update
        QString sItemX; // selected X column
        QString sItemColor; // selected colour-scale column
        std::vector<std::map<QString, PlotSettings>> mapItemsY = {std::map<QString, PlotSettings>(), std::map<QString, PlotSettings>()};

        if(keepOrResetSelection)
        {
            // Store the currently selected plot columns to restore them later
            for(int i = 0; i < ui->treePlotColumns->topLevelItemCount(); ++i)
            {
                QTreeWidgetItem* item = ui->treePlotColumns->topLevelItem(i);
                if(item->checkState(PlotColumnX) == Qt::Checked)
                    sItemX = item->text(PlotColumnField);
                if(item->checkState(PlotColumnColour) == Qt::Checked)
                    sItemColor = item->text(PlotColumnField);

                for(size_t y_ind = 0; y_ind < 2; y_ind++)
                  if(item->checkState(PlotColumnY[y_ind]) == Qt::Checked)
                    mapItemsY[y_ind][item->text(PlotColumnField)] = PlotSettings(
                        0, 0,
                        item->background(PlotColumnY[y_ind]).color(),
                        item->checkState(PlotColumnY[y_ind]) == Qt::Checked);
            }
        } else {
            // Get the plot columns to select from the stored browse table information
            sItemX = m_currentTableSettings->plotXAxis;
            sItemColor = m_currentTableSettings->plotColorColumn;
            mapItemsY[0] = m_currentTableSettings->plotYAxes[0];
            mapItemsY[1] = m_currentTableSettings->plotYAxes[1];

            // Also restore the stored gradient for this table, if any
            if(m_currentTableSettings->plotColorGradient >= 0)
            {
                ui->comboColorGradient->blockSignals(true);
                ui->comboColorGradient->setCurrentIndex(m_currentTableSettings->plotColorGradient);
                ui->comboColorGradient->blockSignals(false);
            }
        }

        ui->treePlotColumns->clear();

        if(model)
        {
            // Add each column with a supported data type to the column selection view
            for(int i=0;i<model->columnCount();++i)
            {
                QVariant::Type columntype = guessDataType(model, i);
                if(columntype != QVariant::Invalid)
                {
                    QTreeWidgetItem* columnitem = new QTreeWidgetItem(ui->treePlotColumns);

                    switch (columntype) {
                    case QVariant::DateTime:
                        columnitem->setText(PlotColumnType, tr("Date/Time"));
                        break;
                    case QVariant::Date:
                        columnitem->setText(PlotColumnType, tr("Date"));
                        break;
                    case QVariant::Time:
                        columnitem->setText(PlotColumnType, tr("Time"));
                        break;
                    case QVariant::Double:
                        columnitem->setText(PlotColumnType, tr("Numeric"));
                        break;
                    case QVariant::String:
                        columnitem->setText(PlotColumnType, tr("Label"));
                        break;
                    default:
                        // This is not actually expected
                        columnitem->setText(PlotColumnType, tr("Invalid"));
                    }

                    // Store the model column index in the PlotColumnField and the type
                    // in the PlotColumnType, both using the User Role.
                    columnitem->setData(PlotColumnField, Qt::UserRole, i);
                    columnitem->setData(PlotColumnType, Qt::UserRole, static_cast<int>(columntype));
                    columnitem->setText(PlotColumnField, model->headerData(i, Qt::Horizontal, Qt::EditRole).toString());

                    // restore previous check state
                    for(size_t y_ind = 0; y_ind < 2; y_ind++)
                      {
                        if(contains(mapItemsY[y_ind], columnitem->text(PlotColumnField)))
                        {
                          columnitem->setCheckState(PlotColumnY[y_ind], mapItemsY[y_ind][columnitem->text(PlotColumnField)].active ? Qt::Checked : Qt::Unchecked);
                          columnitem->setBackground(PlotColumnY[y_ind], mapItemsY[y_ind][columnitem->text(PlotColumnField)].colour);
                        } else {
                        if (columntype == QVariant::Double)
                          columnitem->setCheckState(PlotColumnY[y_ind], Qt::Unchecked);
                        }
                      }

                    if(sItemX == columnitem->text(PlotColumnField))
                        columnitem->setCheckState(PlotColumnX, Qt::Checked);
                    else
                        columnitem->setCheckState(PlotColumnX, Qt::Unchecked);

                    // Points can be coloured by a numeric column (continuous gradient) or by a
                    // label column (one distinct colour per label)
                    if(columntype == QVariant::Double || columntype == QVariant::String)
                    {
                        if(sItemColor == columnitem->text(PlotColumnField))
                            columnitem->setCheckState(PlotColumnColour, Qt::Checked);
                        else
                            columnitem->setCheckState(PlotColumnColour, Qt::Unchecked);
                    }
                }
            }

            ui->treePlotColumns->resizeColumnToContents(PlotColumnField);

            // Add a row number column at the beginning of the column list, but only when there were (other) columns added
            if(ui->treePlotColumns->topLevelItemCount())
            {
                QTreeWidgetItem* columnitem = new QTreeWidgetItem(ui->treePlotColumns);

                // Just set RowNumId in the user role information field here to somehow indicate what column this is
                columnitem->setData(PlotColumnField, Qt::UserRole, RowNumId);
                columnitem->setText(PlotColumnField, tr("Row #"));
                columnitem->setData(PlotColumnType, Qt::UserRole, static_cast<int>(QVariant::Double));
                columnitem->setText(PlotColumnType, tr("Numeric"));

                // restore previous check state
                for(size_t y_ind = 0; y_ind < 2; y_ind++)
                {
                    if(contains(mapItemsY[y_ind], columnitem->text(PlotColumnField)))
                    {
                      columnitem->setCheckState(PlotColumnY[y_ind], mapItemsY[y_ind][columnitem->text(PlotColumnField)].active ? Qt::Checked : Qt::Unchecked);
                      columnitem->setBackground(PlotColumnY[y_ind], mapItemsY[y_ind][columnitem->text(PlotColumnField)].colour);
                    } else {
                    columnitem->setCheckState(PlotColumnY[y_ind], Qt::Unchecked);
                    }
                }

                if(sItemX == columnitem->text(PlotColumnField))
                    columnitem->setCheckState(PlotColumnX, Qt::Checked);
                else
                    columnitem->setCheckState(PlotColumnX, Qt::Unchecked);

                if(sItemColor == columnitem->text(PlotColumnField))
                    columnitem->setCheckState(PlotColumnColour, Qt::Checked);
                else
                    columnitem->setCheckState(PlotColumnColour, Qt::Unchecked);

                ui->treePlotColumns->takeTopLevelItem(ui->treePlotColumns->indexOfTopLevelItem(columnitem));
                ui->treePlotColumns->insertTopLevelItem(0, columnitem);
            }
        }

        yAxes[0]->setLabel("Y1");
        yAxes[1]->setLabel("Y2");
        ui->plotWidget->xAxis->setLabel("X");
        ui->treePlotColumns->blockSignals(false);
    }

    // search for the x axis select
    QTreeWidgetItem* xitem = nullptr;
    for(int i = 0; i < ui->treePlotColumns->topLevelItemCount(); ++i)
    {
        xitem = ui->treePlotColumns->topLevelItem(i);
        if(xitem->checkState(PlotColumnX) == Qt::Checked)
            break;

        xitem = nullptr;
    }

    std::vector<QStringList> yAxisLabels = {QStringList(), QStringList()};

    // State of the optional point colouring (set while drawing below, applied to the legend afterwards)
    bool colorActive = false;       // a colour column is in use
    bool categoricalColor = false;  // the colour column is a label column (one colour per label)
    QCPRange colorRange;            // value range for the continuous (numeric) gradient
    QString colorLabel;             // colour-bar label (the numeric column name)

    // Clear graphs and axis labels
    ui->plotWidget->clearPlottables();
    ui->plotWidget->xAxis->setLabel(QString());
    yAxes[0]->setLabel(QString());
    yAxes[1]->setLabel(QString());

    if(xitem)
    {
        // regain the model column index and the datatype
        // right now datatype is only important for X axis (Y is always numeric)
        int x = xitem->data(PlotColumnField, Qt::UserRole).toInt();
        m_xtype = xitem->data(PlotColumnType, Qt::UserRole).toUInt();

        ui->plotWidget->xAxis->setTickLabelRotation(0);

        // check if we have a x axis with datetime data
        switch (m_xtype) {
        case QVariant::Date: {
            QSharedPointer<QCPAxisTickerDateTime> ticker(new QCPAxisTickerDateTime);
            ticker->setDateTimeFormat("yyyy-MM-dd");
            ui->plotWidget->xAxis->setTicker(ticker);
            break;
        }
        case QVariant::DateTime: {
            QSharedPointer<QCPAxisTickerDateTime> ticker(new QCPAxisTickerDateTime);
            ticker->setDateTimeFormat("yyyy-MM-dd\nhh:mm:ss");
            ui->plotWidget->xAxis->setTicker(ticker);
            break;
        }
        case QVariant::Time: {
            QSharedPointer<QCPAxisTickerDateTime> ticker(new QCPAxisTickerDateTime);
            ticker->setDateTimeFormat("hh:mm:ss");
            ticker->setDateTimeSpec(Qt::UTC);
            ui->plotWidget->xAxis->setTicker(ticker);
            break;
        }
        case QVariant::String: {
            // Ticker is set when we have got the labels
            ui->plotWidget->xAxis->setTickLabelRotation(60);
            break;
        }
        default: {
            QSharedPointer<QCPAxisTickerFixed> ticker(new QCPAxisTickerFixed);
            ticker->setTickStepStrategy(QCPAxisTicker::tssReadability);
            ticker->setScaleStrategy(QCPAxisTickerFixed::ssMultiples);
            ui->plotWidget->xAxis->setTicker(ticker);
        }
        }

        // Boolean to decide whether secondary y axis should be displayed
        bool displayY2Axis = false;

        // Determine whether a column was selected to colour the points. Colouring is not applied to
        // bar charts (String X type). A numeric colour column maps every point onto a continuous
        // gradient; a label (String) colour column gives every distinct label its own colour.
        QTreeWidgetItem* colorItem = checkedItem(PlotColumnColour);
        const unsigned int colorType = colorItem ? colorItem->data(PlotColumnType, Qt::UserRole).toUInt() : QVariant::Invalid;
        colorActive = colorItem && m_xtype != QVariant::String &&
                      (colorType == QVariant::Double || colorType == QVariant::String);
        categoricalColor = colorActive && colorType == QVariant::String;

        // Per-point data shared by all y-series. For numeric colouring we keep the value and its
        // range; for categorical colouring we keep each row's label and the label-to-colour mapping.
        QVector<double> colorData;
        QVector<QString> rowLabels;
        std::map<QString, QColor> labelColors;
        bool categoricalLegendDone = false;

        if(colorActive && !categoricalColor)
        {
            const int colorColumn = colorItem->data(PlotColumnField, Qt::UserRole).toInt();
            colorLabel = (colorColumn == RowNumId) ? tr("Row #")
                                                   : model->headerData(colorColumn, Qt::Horizontal, Qt::EditRole).toString();
            const int nrows = model->rowCount();
            colorData.resize(nrows);

            bool first = true;
            for(int j = 0; j < nrows; ++j)
            {
                QVariant v = (colorColumn == RowNumId) ? QVariant(j+1) : model->data(model->index(j, colorColumn), Qt::EditRole);
                if(v.isNull())
                {
                    colorData[j] = qQNaN();
                    continue;
                }
                const double d = v.toDouble();
                colorData[j] = d;
                if(first)
                {
                    colorRange.lower = colorRange.upper = d;
                    first = false;
                } else {
                    colorRange.lower = std::min(colorRange.lower, d);
                    colorRange.upper = std::max(colorRange.upper, d);
                }
            }
            // Guard against a zero-width range (all values equal), which would make the gradient undefined
            if(colorRange.lower == colorRange.upper)
                colorRange.upper = colorRange.lower + 1.0;
        }
        else if(categoricalColor)
        {
            // Limit the number of distinct labels we colour, to keep the legend readable and the
            // number of overlay graphs bounded. Extra labels keep the series base colour.
            const int maxCategories = 100;
            const int colorColumn = colorItem->data(PlotColumnField, Qt::UserRole).toInt();
            const int nrows = model->rowCount();
            rowLabels.resize(nrows);

            std::set<QString> distinct;
            for(int j = 0; j < nrows; ++j)
            {
                const QVariant v = model->data(model->index(j, colorColumn), Qt::EditRole);
                if(v.isNull())
                {
                    rowLabels[j] = QString();    // null string marks "no label"
                    continue;
                }
                const QString label = v.toString();
                rowLabels[j] = label;
                if(static_cast<int>(distinct.size()) < maxCategories)
                    distinct.insert(label);
            }

            // Assign a colour to each label by sampling the selected gradient at evenly spaced
            // positions, so distinct labels get distinct colours from the chosen palette.
            QCPColorGradient gradient = gradientFromIndex(ui->comboColorGradient->currentIndex());
            const QCPRange sampleRange(0, std::max<int>(static_cast<int>(distinct.size()), 1));
            int idx = 0;
            for(const QString& label : distinct)
                labelColors[label] = QColor(gradient.color(idx++, sampleRange));
        }

        // add graph for each selected y axis
        for(int i = 0; i < ui->treePlotColumns->topLevelItemCount(); ++i)
        {
            QTreeWidgetItem* item = ui->treePlotColumns->topLevelItem(i);
            std::vector<bool> yItemBool = {false, false};

            if(item->checkState((PlotColumnY[0])) == Qt::Checked || item->checkState((PlotColumnY[1])) == Qt::Checked)
            {
                for(size_t y_ind = 0; y_ind < 2; y_ind++)
                    if(item->checkState((PlotColumnY[y_ind])) == Qt::Checked)
                        yItemBool[y_ind] = true;

                if(yItemBool[1])
                    displayY2Axis = true;
                // regain the model column index
                int column = item->data(PlotColumnField, Qt::UserRole).toInt();

                bool isSorted = true;

                // prepare the data vectors for qcustomplot
                // possible improvement might be a QVector subclass that directly
                // access the model data, to save memory, we are copying here

                auto nrows = model->rowCount();

                std::vector<QVector<double>> ydata{QVector<double>(nrows), QVector<double>(nrows)};

                QVector<double> xdata(nrows), tdata(nrows);
                QVector<QString> labels;

                for(int j = 0; j < nrows; ++j)
                {
                    tdata[j] = j;

                    if(x != RowNumId && model->data(model->index(j, x), Qt::EditRole).isNull()) {
                        // NULL values produce gaps in the linear graphs. We use NaN values in
                        // that case as required by QCustomPlot.
                        // Bar plots will display the configured string for NULL values.
                        if(m_xtype == QVariant::String) {
                            xdata[j] = j+1;
                            labels << model->data(model->index(j, x), Qt::DisplayRole).toString();
                        } else
                            xdata[j] = qQNaN();
                    } else {

                        // convert x type axis if it's datetime
                        switch (m_xtype) {
                        case QVariant::DateTime:
                        case QVariant::Date: {
                            QString s = model->data(model->index(j, x)).toString();
                            QDateTime d = QDateTime::fromString(s, Qt::ISODate);
                            xdata[j] = static_cast<double>(d.toMSecsSinceEpoch()) / 1000.0;
                            break;
                        }
                        case QVariant::Time: {
                            QString s = model->data(model->index(j, x)).toString();
                            QTime t = QTime::fromString(s);
                            xdata[j] = t.msecsSinceStartOfDay() / 1000.0;
                            break;
                        }
                        case QVariant::String: {
                            xdata[j] = j+1;
                            labels << model->data(model->index(j, x)).toString();
                            break;
                        }
                        default: {
                            // Get the x value for this point. If the selected column is -1, i.e. the row number, just use the current row number from the loop
                            // instead of retrieving some value from the model.
                            if(x == RowNumId)
                                xdata[j] = j+1;
                            else
                                xdata[j] = model->data(model->index(j, x)).toDouble();
                        }
                        }
                    }
                    if (j != 0)
                        isSorted &= (xdata[j-1] <= xdata[j]);

                    // Get the y value for this point. If the selected column is -1, i.e. the row number, just use the current row number from the loop
                    // instead of retrieving some value from the model.
                    QVariant pointdata;
                    if(column == RowNumId)
                        pointdata = j+1;
                    else
                        pointdata = model->data(model->index(j, column), Qt::EditRole);

                    for(size_t y_ind = 0; y_ind < 2; y_ind++)
                    {
                        if(pointdata.isNull()){
                            if(yItemBool[y_ind])
                                ydata[y_ind][j] = qQNaN();
                        }
                        else{
                            if(yItemBool[y_ind])
                                ydata[y_ind][j] = pointdata.toDouble();
                        }
                    }
                }

                // Line type and point shape are not supported by the String X type (Bars)
                ui->comboLineType->setEnabled(m_xtype != QVariant::String);
                ui->comboPointShape->setEnabled(m_xtype != QVariant::String);

                // WARN: ssDot is removed
                int shapeIdx = ui->comboPointShape->currentIndex();
                if (shapeIdx > 0) shapeIdx += 1;
                // When colour-scaling the points we need a visible marker to carry the colour, so fall
                // back to a disc when the user left the point shape on "None".
                if (colorActive && shapeIdx == QCPScatterStyle::ssNone)
                    shapeIdx = QCPScatterStyle::ssDisc;
                QCPScatterStyle scatterStyle = QCPScatterStyle(static_cast<QCPScatterStyle::ScatterShape>(shapeIdx), 5);

                QCPAbstractPlottable* plottable = nullptr;
                // When the X type is String, we draw a bar chart.
                // When it is already sorted by x, we draw a graph.
                // When it is not sorted by x, we draw a curve, so the order selected by the user in the table or in the query is
                // respected.  In this case the line will have loops and only None and Line is supported as line style.
                // TODO: how to make the user aware of this without disturbing.
                if (m_xtype == QVariant::String) {

                    for(size_t y_ind = 0; y_ind < 2; y_ind++)
                    {
                        if(yItemBool[y_ind])
                        {
                            QCPBars* bars = new QCPBars(ui->plotWidget->xAxis, yAxes[y_ind]);
                            plottable = bars;
                            bars->setData(xdata, ydata[y_ind]);
                            // Set ticker once
                            if (ui->plotWidget->plottableCount() == 1) {
                                QSharedPointer<QCPAxisTickerText> ticker(new QCPAxisTickerText);
                                ticker->addTicks(xdata, labels);
                                ui->plotWidget->xAxis->setTicker(ticker);
                            }
                            QColor color = item->background(PlotColumnY[y_ind]).color();
                            bars->setBrush(color);
                            plottable->setPen(QPen(color.darker(150)));
                        }
                    }
                } else {
                    if (isSorted) {
                        for(size_t y_ind = 0; y_ind < 2; y_ind++)
                        {
                            if(yItemBool[y_ind])
                            {
                                QCPGraph* graph = ui->plotWidget->addGraph(ui->plotWidget->xAxis, yAxes[y_ind]);
                                plottable = graph;
                                graph->setData(xdata, ydata[y_ind], /*alreadySorted*/ true);
                                // set some graph styles not supported by the abstract plottable
                                graph->setLineStyle(static_cast<QCPGraph::LineStyle>(ui->comboLineType->currentIndex()));
                                graph->setScatterStyle(scatterStyle);
                                plottable->setPen(QPen(item->background(PlotColumnY[y_ind]).color()));
                            }
                        }
                    } else {
                        for(size_t y_ind = 0; y_ind < 2; y_ind++)
                        {
                            if(yItemBool[y_ind])
                            {
                                QCPCurve* curve = new QCPCurve(ui->plotWidget->xAxis, yAxes[y_ind]);
                                plottable = curve;
                                curve->setData(tdata, xdata, ydata[y_ind], /*alreadySorted*/ true);
                                // set some curve styles not supported by the abstract plottable
                                if (ui->comboLineType->currentIndex() == QCPCurve::lsNone)
                                    curve->setLineStyle(QCPCurve::lsNone);
                                else
                                    curve->setLineStyle(QCPCurve::lsLine);
                                curve->setScatterStyle(scatterStyle);
                                plottable->setPen(QPen(item->background(PlotColumnY[y_ind]).color()));
                            }
                        }
                    }

                    // Overlay coloured points on top of the just-created graphs/curves. The
                    // underlying plottable keeps its (single colour) markers so it stays selectable,
                    // while these opaque coloured points sit exactly on top of them.
                    if(colorActive)
                    {
                        for(size_t y_ind = 0; y_ind < 2; y_ind++)
                        {
                            if(!yItemBool[y_ind])
                                continue;
                            if(categoricalColor)
                            {
                                // Only the first drawn series populates the legend, so each label
                                // appears exactly once even with two y-axes.
                                drawCategoricalPoints(yAxes[y_ind], xdata, ydata[y_ind], rowLabels, labelColors, shapeIdx, !categoricalLegendDone);
                                categoricalLegendDone = true;
                            } else {
                                drawColorScaledPoints(yAxes[y_ind], xdata, ydata[y_ind], colorData, colorRange, shapeIdx);
                            }
                        }
                    }
                }

                if(plottable)
                {
                    plottable->setSelectable(QCP::stDataRange);
                    plottable->setName(item->text(PlotColumnField));
                    // In categorical colouring the legend lists the labels, so keep the series
                    // plottables themselves out of it to avoid clutter.
                    if(categoricalColor)
                        plottable->removeFromLegend();
                }

                for(size_t y_ind = 0; y_ind < 2; y_ind++)
                {
                    if(yItemBool[y_ind])
                    {
                        // gather Y label column names
                        if(column == RowNumId)
                            yAxisLabels[y_ind] << tr("Row #");
                        else
                            yAxisLabels[y_ind] << model->headerData(column, Qt::Horizontal, Qt::EditRole).toString();
                    }
                }
            }
        }

        ui->plotWidget->rescaleAxes(true);
        // The label legend is the whole point of categorical colouring, so force it visible then.
        ui->plotWidget->legend->setVisible(m_showLegend || categoricalColor);
        // Legend with slightly transparent background brush:
        ui->plotWidget->legend->setBrush(QColor(255, 255, 255, 150));

        // set axis labels
        if(x == RowNumId)
            ui->plotWidget->xAxis->setLabel(tr("Row #"));
        else
            ui->plotWidget->xAxis->setLabel(model->headerData(x, Qt::Horizontal, Qt::EditRole).toString());
        for(size_t y_ind = 0; y_ind < 2; y_ind++)
            yAxes[y_ind]->setLabel(yAxisLabels[y_ind].join("|"));

        if(displayY2Axis){
          yAxes[1]->setVisible(true);
          yAxes[1]->setTickLabels(true);
        }else{
          yAxes[1]->setVisible(false);
          yAxes[1]->setTickLabels(false);
        }
    }

    // The continuous colour bar is only shown for numeric colouring. Rebuild it from scratch on
    // every update so it can never get out of sync or be left behind when colouring is switched
    // off or changed (categorical colouring uses the normal legend instead).
    if(m_colorScale)
    {
        ui->plotWidget->plotLayout()->remove(m_colorScale);   // remove() also deletes the element
        ui->plotWidget->plotLayout()->simplify();
        m_colorScale = nullptr;
    }
    if(colorActive && !categoricalColor)
    {
        m_colorScale = new QCPColorScale(ui->plotWidget);
        m_colorScale->setType(QCPAxis::atRight);
        m_colorScale->setGradient(gradientFromIndex(ui->comboColorGradient->currentIndex()));
        m_colorScale->setDataRange(colorRange);
        m_colorScale->axis()->setLabel(colorLabel);
        ui->plotWidget->plotLayout()->addElement(0, 1, m_colorScale);

        // Keep the colour bar vertically aligned with the main axis rect
        if(!m_colorScaleMarginGroup)
            m_colorScaleMarginGroup = new QCPMarginGroup(ui->plotWidget);
        ui->plotWidget->axisRect()->setMarginGroup(QCP::msTop | QCP::msBottom, m_colorScaleMarginGroup);
        m_colorScale->setMarginGroup(QCP::msTop | QCP::msBottom, m_colorScaleMarginGroup);
    }

    adjustBars();
    adjustAxisFormat();
    ui->plotWidget->replot();

    // Warn user if not all data has been fetched and hint about the button for loading all the data
    if (model && (model->rowCountAvailable() != SqliteTableModel::RowCount::Complete || !model->isCacheComplete())) {
        ui->buttonLoadAllData->setEnabled(true);
        ui->buttonLoadAllData->setStyleSheet("QToolButton {color: white; background-color: rgb(255, 102, 102)}");
        ui->buttonLoadAllData->setToolTip(tr("Load all data and redraw plot.\n"
                                             "Warning: not all data has been fetched from the table yet due to the partial fetch mechanism."));
    } else {
        ui->buttonLoadAllData->setEnabled(false);
        ui->buttonLoadAllData->setStyleSheet("");
        ui->buttonLoadAllData->setToolTip(tr("Load all data and redraw plot"));
    }
}

void PlotDock::resetPlot()
{
    updatePlot(nullptr);
}

void PlotDock::columnItemChanged(QTreeWidgetItem* changeitem, int column)
{
    // disable change updates, or we get unwanted redrawing and weird behavior
    ui->treePlotColumns->blockSignals(true);

    // make sure only 1 X axis is selected
    if(column == PlotColumnX)
    {
        for(int i = 0; i < ui->treePlotColumns->topLevelItemCount(); ++i)
        {
            QTreeWidgetItem* item = ui->treePlotColumns->topLevelItem(i);
            if(item->checkState(column) == Qt::Checked && item != changeitem)
            {
                item->setCheckState(column, Qt::Unchecked);
            }
        }

        // Save settings for this table
        if(m_currentTableSettings)
        {
            if(changeitem->checkState(column) == Qt::Checked)
                m_currentTableSettings->plotXAxis = changeitem->text(PlotColumnField);
            else
                m_currentTableSettings->plotXAxis = QString();
        }
    } else if(column == PlotColumnColour) {
        // make sure only 1 colour column is selected
        for(int i = 0; i < ui->treePlotColumns->topLevelItemCount(); ++i)
        {
            QTreeWidgetItem* item = ui->treePlotColumns->topLevelItem(i);
            if(item->checkState(column) == Qt::Checked && item != changeitem)
                item->setCheckState(column, Qt::Unchecked);
        }

        // Save settings for this table
        if(m_currentTableSettings)
        {
            if(changeitem->checkState(column) == Qt::Checked)
            {
                m_currentTableSettings->plotColorColumn = changeitem->text(PlotColumnField);
                m_currentTableSettings->plotColorGradient = ui->comboColorGradient->currentIndex();
            }
            else
                m_currentTableSettings->plotColorColumn = QString();
        }
    } else if(column == PlotColumnY[0] || column == PlotColumnY[1]) {
        // Save check state of this column
        for(size_t y_ind = 0; y_ind < 2; y_ind++)
        {
            if(column == PlotColumnY[y_ind])
            {
                if(m_currentTableSettings)
                {
                    PlotSettings& plot_settings = m_currentTableSettings->plotYAxes[y_ind][changeitem->text(PlotColumnField)];
                    plot_settings.active = (changeitem->checkState(column) == Qt::Checked);
                }

                if(changeitem->checkState(column) == Qt::Checked)
                {
                    // Generate a default colour if none is set yet
                    QColor colour = changeitem->background(column).color();
                    if(!colour.isValid() || colour == changeitem->background(PlotColumnField).color())
                        colour = m_graphPalette.nextSerialColor(true);

                    // Set colour to cell background
                    changeitem->setBackground(column, colour);

                    // Save settings for this table
                    if(m_currentTableSettings)
                    {
                        PlotSettings& plot_settings = m_currentTableSettings->plotYAxes[y_ind][changeitem->text(PlotColumnField)];
                        plot_settings.colour = colour;
                        plot_settings.lineStyle = ui->comboLineType->currentIndex();
                        plot_settings.pointShape = (ui->comboPointShape->currentIndex() > 0 ? (ui->comboPointShape->currentIndex()+1) : ui->comboPointShape->currentIndex());
                    }
                }
            }
        }

    }

    ui->treePlotColumns->blockSignals(false);

    updatePlot(m_currentPlotModel, m_currentTableSettings, false);
}

void PlotDock::columnItemDoubleClicked(QTreeWidgetItem* item, int column)
{
    // disable change updates, or we get unwanted redrawing and weird behavior
    ui->treePlotColumns->blockSignals(true);

    unsigned int type = item->data(PlotColumnType, Qt::UserRole).toUInt();

    for(size_t y_ind = 0; y_ind < 2; y_ind++)
    {
        if(column == PlotColumnY[y_ind] && type == QVariant::Double)
        {
            // On double click open the colordialog
            QColorDialog colordialog(this);
            QColor curbkcolor = item->background(column).color();
            QColor precolor = !curbkcolor.isValid() ? static_cast<Qt::GlobalColor>(random_number(5, 13)) : curbkcolor;
            QColor color = colordialog.getColor(precolor, this, tr("Choose an axis color"));
            if(color.isValid())
            {
                item->setCheckState(column, Qt::Checked);
                item->setBackground(column, color);

                // Save settings for this table
                if(m_currentTableSettings)
                {
                    PlotSettings& plot_settings = m_currentTableSettings->plotYAxes[y_ind][item->text(PlotColumnField)];
                    plot_settings.active = (item->checkState(column) == Qt::Checked);
                    plot_settings.colour = color;
                    plot_settings.lineStyle = ui->comboLineType->currentIndex();
                    plot_settings.pointShape = (ui->comboPointShape->currentIndex() > 0 ? (ui->comboPointShape->currentIndex()+1) : ui->comboPointShape->currentIndex());
                }
            } else {
                item->setCheckState(column, Qt::Unchecked);

                // Save settings for this table
                if(m_currentTableSettings)
                    m_currentTableSettings->plotYAxes[y_ind].erase(item->text(PlotColumnField));
            }
        }
    }

    ui->treePlotColumns->blockSignals(false);

    updatePlot(m_currentPlotModel, m_currentTableSettings, false);
}

void PlotDock::savePlot()
{
    QString fileName = FileDialog::getSaveFileName(
                           CreateDataFile,
                           this,
                           tr("Choose a filename to save under"),
                           tr("PNG(*.png);;JPG(*.jpg);;PDF(*.pdf);;BMP(*.bmp);;All Files(*)"));
    if(!fileName.isEmpty())
    {
        if(fileName.endsWith(".png", Qt::CaseInsensitive))
        {
            ui->plotWidget->savePng(fileName);
        }
        else if(fileName.endsWith(".jpg", Qt::CaseInsensitive))
        {
            ui->plotWidget->saveJpg(fileName);
        }
        else if(fileName.endsWith(".pdf", Qt::CaseInsensitive))
        {
            ui->plotWidget->savePdf(fileName);
        }
        else if(fileName.endsWith(".bmp", Qt::CaseInsensitive))
        {
            ui->plotWidget->saveBmp(fileName);
        }
        else
        {
            fileName += ".png";
            ui->plotWidget->savePng(fileName);
        }
    }
}

void PlotDock::lineTypeChanged(int index)
{
    Q_ASSERT(index >= QCPGraph::lsNone &&
             index <= QCPGraph::lsImpulse);

    bool hasCurves = (ui->plotWidget->plottableCount() > ui->plotWidget->graphCount());
    QCPGraph::LineStyle lineStyle = static_cast<QCPGraph::LineStyle>(index);
    if (lineStyle > QCPGraph::lsLine && hasCurves) {
        QMessageBox::warning(this, qApp->applicationName(),
                             tr("There are curves in this plot and the selected line style can only be applied to graphs sorted by X. "
                                "Either sort the table or query by X to remove curves or select one of the styles supported by curves: "
                                "None or Line."));
        return;
    }
    for (int i = 0, ie = ui->plotWidget->graphCount(); i < ie; ++i)
    {
        QCPGraph * graph = ui->plotWidget->graph(i);
        if (graph)
            graph->setLineStyle(lineStyle);
    }
    // We have changed the style only for graphs, but not for curves.
    // If there are any in the plot, we have to update it completely in order to apply the new style.
    // The same is true when colour-scaling is active, since the manual loop above also touches the
    // (line-less) coloured overlay graphs which then need to be rebuilt.
    if (hasCurves || checkedItem(PlotColumnColour))
        updatePlot(m_currentPlotModel, m_currentTableSettings, false);
    else
        ui->plotWidget->replot();

    // Save settings for this table
    if(m_currentTableSettings)
    {
        for(size_t y_ind = 0; y_ind < 2; y_ind++)
        {
            std::map<QString, PlotSettings>& graphs = m_currentTableSettings->plotYAxes[y_ind];
            auto it = graphs.begin();
            while(it != graphs.end())
            {
                it->second.lineStyle = lineStyle;
                ++it;
            }
        }
    }
}

void PlotDock::pointShapeChanged(int index)
{
    // WARN: because ssDot point shape is removed
    if (index > 0) index += 1;
    Q_ASSERT(index >= QCPScatterStyle::ssNone &&
             index <  QCPScatterStyle::ssPixmap);

    bool hasCurves = (ui->plotWidget->plottableCount() > ui->plotWidget->graphCount());
    QCPScatterStyle::ScatterShape shape = static_cast<QCPScatterStyle::ScatterShape>(index);
    for (int i = 0, ie = ui->plotWidget->graphCount(); i < ie; ++i)
    {
        QCPGraph * graph = ui->plotWidget->graph(i);
        if (graph)
            graph->setScatterStyle(QCPScatterStyle(shape, 5));
    }
    // We have changed the style only for graphs, but not for curves.
    // If there are any in the plot, we have to update it completely in order to apply the new style.
    // The same is true when colour-scaling is active, since the manual loop above strips the colour
    // off the overlay points which then need to be rebuilt.
    if (hasCurves || checkedItem(PlotColumnColour))
        updatePlot(m_currentPlotModel, m_currentTableSettings, false);
    else
        ui->plotWidget->replot();

    // Save settings for this table
    if(m_currentTableSettings)
    {
        for(size_t y_ind = 0; y_ind < 2; y_ind++)
        {
            std::map<QString, PlotSettings>& graphs = m_currentTableSettings->plotYAxes[y_ind];
            auto it = graphs.begin();
            while(it != graphs.end())
            {
                it->second.pointShape = shape;
                ++it;
            }
        }
    }
}

void PlotDock::colorGradientChanged(int index)
{
    // Persist the gradient as the global default and, if a colour column is in use, for this table
    Settings::setValue("PlotDock", "colorGradient", index);
    if(m_currentTableSettings && checkedItem(PlotColumnColour))
        m_currentTableSettings->plotColorGradient = index;

    // Rebuild so the points and the colour-scale legend pick up the new gradient
    updatePlot(m_currentPlotModel, m_currentTableSettings, false);
}

QTreeWidgetItem* PlotDock::checkedItem(int column) const
{
    for(int i = 0; i < ui->treePlotColumns->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* item = ui->treePlotColumns->topLevelItem(i);
        if(item->checkState(column) == Qt::Checked)
            return item;
    }
    return nullptr;
}

void PlotDock::drawColorScaledPoints(QCPAxis* valueAxis, const QVector<double>& xdata, const QVector<double>& ydata,
                                     const QVector<double>& colorData, const QCPRange& range, int shape)
{
    QCPColorGradient gradient = gradientFromIndex(ui->comboColorGradient->currentIndex());

    // Group the points by the exact colour the gradient maps them to. This keeps the number of
    // overlay graphs small (at most one per distinct colour) while matching the colour-scale legend.
    std::map<QRgb, QVector<double>> xByColor;
    std::map<QRgb, QVector<double>> yByColor;

    const int n = std::min(colorData.size(), std::min(xdata.size(), ydata.size()));
    for(int j = 0; j < n; ++j)
    {
        // Points without a colour value (or without coordinates) are left to the underlying plottable
        if(qIsNaN(colorData[j]) || qIsNaN(xdata[j]) || qIsNaN(ydata[j]))
            continue;

        const QRgb rgb = gradient.color(colorData[j], range);
        xByColor[rgb].append(xdata[j]);
        yByColor[rgb].append(ydata[j]);
    }

    for(const auto& entry : xByColor)
    {
        const QRgb rgb = entry.first;
        const QColor color(rgb);

        QCPGraph* overlay = ui->plotWidget->addGraph(ui->plotWidget->xAxis, valueAxis);
        overlay->setData(entry.second, yByColor[rgb], /*alreadySorted*/ false);
        overlay->setLineStyle(QCPGraph::lsNone);
        overlay->setScatterStyle(QCPScatterStyle(static_cast<QCPScatterStyle::ScatterShape>(shape), color, color, 5));
        overlay->setPen(QPen(color));
        // These points are purely decorative: keep them out of the selection and the legend so the
        // underlying plottable remains the single source of truth for point/row selection.
        overlay->setSelectable(QCP::stNone);
        overlay->removeFromLegend();
    }
}

void PlotDock::drawCategoricalPoints(QCPAxis* valueAxis, const QVector<double>& xdata, const QVector<double>& ydata,
                                     const QVector<QString>& rowLabels, const std::map<QString, QColor>& labelColors,
                                     int shape, bool addToLegend)
{
    // Group the points by their label, so each distinct label becomes a single overlay graph.
    std::map<QString, QVector<double>> xByLabel;
    std::map<QString, QVector<double>> yByLabel;

    const int n = std::min(rowLabels.size(), std::min(xdata.size(), ydata.size()));
    for(int j = 0; j < n; ++j)
    {
        // Rows without a label (NULL) or coordinates are left to the underlying plottable.
        // Labels beyond the colouring limit are not in labelColors and are skipped too.
        if(rowLabels[j].isNull() || qIsNaN(xdata[j]) || qIsNaN(ydata[j]))
            continue;
        if(labelColors.find(rowLabels[j]) == labelColors.end())
            continue;

        xByLabel[rowLabels[j]].append(xdata[j]);
        yByLabel[rowLabels[j]].append(ydata[j]);
    }

    for(const auto& entry : xByLabel)
    {
        const QString& label = entry.first;
        const QColor color = labelColors.at(label);

        QCPGraph* overlay = ui->plotWidget->addGraph(ui->plotWidget->xAxis, valueAxis);
        overlay->setData(entry.second, yByLabel[label], /*alreadySorted*/ false);
        overlay->setLineStyle(QCPGraph::lsNone);
        overlay->setScatterStyle(QCPScatterStyle(static_cast<QCPScatterStyle::ScatterShape>(shape), color, color, 5));
        overlay->setPen(QPen(color));
        overlay->setName(label);
        // Purely decorative: not selectable, so the underlying plottable remains the single source
        // of truth for point/row selection. Keep the legend entry only when requested.
        overlay->setSelectable(QCP::stNone);
        if(!addToLegend)
            overlay->removeFromLegend();
    }
}

QVariant::Type PlotDock::guessDataType(SqliteTableModel* model, int column) const
{
    QVariant::Type type = QVariant::Invalid;
    for(int i = 0; i < std::min(10, model->rowCount()) && type != QVariant::String; ++i)
    {
        QVariant varData = model->data(model->index(i, column), Qt::EditRole);
        if(varData.isNull() || varData.convert(QVariant::Double))
        {
            type = QVariant::Double;
        } else {
            QString s = model->data(model->index(i, column)).toString();
            QDateTime dt = QDateTime::fromString(s, Qt::ISODate);
            QTime t = QTime::fromString(s);
            if (dt.isValid())
                // Since the way to discriminate dates with times and pure dates is that the time part is 0, we must take into account
                // that some DateTimes could have "00:00:00" as time part and still the entire column has time information, so a single
                // final Date should not set the type to Date if it has already been guessed as DateTime.
                if (type != QVariant::DateTime && dt.time().msecsSinceStartOfDay() == 0)
                    type = QVariant::Date;
                else
                    type = QVariant::DateTime;
            else if (t.isValid())
                type = QVariant::Time;
            else
                type = QVariant::String;
        }
    }

    return type;
}

void PlotDock::fetchAllData()
{
    if(m_currentPlotModel)
    {
#ifdef LOAD_DATA_BENCHMARK
    // If benchmark mode is enabled start measuring the performance now
    QElapsedTimer timer;
    timer.start();
#endif

        // Make sure all data is loaded
        m_currentPlotModel->completeCache();

#ifdef LOAD_DATA_BENCHMARK
    QMessageBox::information(this, qApp->applicationName(),
                             tr("Loading all remaining data for this table took %1ms.")
                             .arg(timer.elapsed()));
#endif

        // Update plot
        updatePlot(m_currentPlotModel, m_currentTableSettings);
    }
}

void PlotDock::selectionChanged()
{

    for (const QCPAbstractPlottable* plottable : ui->plotWidget->selectedPlottables()) {

        for (const QCPDataRange& dataRange : plottable->selection().dataRanges()) {

            int index = dataRange.begin();
            if (dataRange.length() != 0) {
                emit pointsSelected(index, dataRange.length());
                break;
            }

        }
    }

}
void PlotDock::mousePress()
{
    // Allow user to reset the plot
    ui->buttonLoadAllData->setEnabled(true);

    // if an axis (or axis labels) is selected, only allow the direction of that axis to be dragged
    // if no axis (or axis labels) is selected, all three axes may be dragged
    if (ui->plotWidget->xAxis->selectedParts().testFlag(QCPAxis::spAxis) ||
        ui->plotWidget->xAxis->selectedParts().testFlag(QCPAxis::spTickLabels) ||
        ui->plotWidget->xAxis->selectedParts().testFlag(QCPAxis::spAxisLabel))
      {
        QList< QCPAxis *> axList = {ui->plotWidget->xAxis};
        ui->plotWidget->axisRect()->setRangeDragAxes(axList);
      }
    else if (yAxes[0]->selectedParts().testFlag(QCPAxis::spAxis) ||
             yAxes[0]->selectedParts().testFlag(QCPAxis::spTickLabels) ||
             yAxes[0]->selectedParts().testFlag(QCPAxis::spAxisLabel))
    {
      QList< QCPAxis *> axList = {yAxes[0]};
      ui->plotWidget->axisRect()->setRangeDragAxes(axList);
    }
    else if (yAxes[1]->selectedParts().testFlag(QCPAxis::spAxis) ||
             yAxes[1]->selectedParts().testFlag(QCPAxis::spTickLabels) ||
             yAxes[1]->selectedParts().testFlag(QCPAxis::spAxisLabel))
      {
      QList< QCPAxis *> axList = {yAxes[1]};
      ui->plotWidget->axisRect()->setRangeDragAxes(axList);
      }
    else{
      QList< QCPAxis *> axList = {ui->plotWidget->xAxis,yAxes[0], yAxes[1]};
      ui->plotWidget->axisRect()->setRangeDragAxes(axList);
    }
}

void PlotDock::mouseMove(QMouseEvent* event)
{
    double x = ui->plotWidget->xAxis->pixelToCoord(event->pos().x());
    double y = ui->plotWidget->yAxis->pixelToCoord(event->pos().y());

    QString xLabel;

    switch (m_xtype) {
    case QVariant::Date:
        xLabel = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(x*1000.0)).toString("yyyy-MM-dd, ");
        break;
    case QVariant::Time:
        xLabel = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(x*1000.0), Qt::UTC).toString("hh:mm:ss, ");
        break;
    case QVariant::DateTime:
        xLabel = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(x*1000.0)).toString("yyyy-MM-dd hh:mm:ss, ");
        break;
    case QVariant::String:
        // In this case, only the y value is displayed
        xLabel = QString("y=");
        break;
    default:
        xLabel = QLocale().toString(x) + ", ";
    }

    ui->plotWidget->setToolTip(xLabel + QLocale().toString(y));
}

void PlotDock::mouseWheel()
{
    // Allow user to reset the plot
    ui->buttonLoadAllData->setEnabled(true);

    // if an axis (or axis labels) is selected, only allow the direction of that axis to be zoomed
    // if no axis (or axis labels) is selected, all three axes may be zoomed
    if (ui->plotWidget->xAxis->selectedParts().testFlag(QCPAxis::spAxis) ||
        ui->plotWidget->xAxis->selectedParts().testFlag(QCPAxis::spTickLabels) ||
        ui->plotWidget->xAxis->selectedParts().testFlag(QCPAxis::spAxisLabel))
      {
        QList< QCPAxis *> axList = {ui->plotWidget->xAxis};
        ui->plotWidget->axisRect()->setRangeZoomAxes(axList);
      }
    else if (yAxes[0]->selectedParts().testFlag(QCPAxis::spAxis) ||
             yAxes[0]->selectedParts().testFlag(QCPAxis::spTickLabels) ||
             yAxes[0]->selectedParts().testFlag(QCPAxis::spAxisLabel))
    {
      QList< QCPAxis *> axList = {yAxes[0]};
      ui->plotWidget->axisRect()->setRangeZoomAxes(axList);
    }
    else if (yAxes[1]->selectedParts().testFlag(QCPAxis::spAxis) ||
             yAxes[1]->selectedParts().testFlag(QCPAxis::spTickLabels) ||
             yAxes[1]->selectedParts().testFlag(QCPAxis::spAxisLabel))
      {
        QList< QCPAxis *> axList = {yAxes[1]};
        ui->plotWidget->axisRect()->setRangeZoomAxes(axList);
      }

    else{
      QList< QCPAxis *> axList = {ui->plotWidget->xAxis,yAxes[0], yAxes[1]};
      ui->plotWidget->axisRect()->setRangeZoomAxes(axList);
    }
}

void PlotDock::copy()
{
    QApplication::clipboard()->setPixmap(ui->plotWidget->toPixmap());
}

void PlotDock::toggleLegendVisible(bool visible)
{
    m_showLegend = visible;
    ui->plotWidget->legend->setVisible(m_showLegend);
    ui->plotWidget->replot();
}

// Stack or group bars and set the appropriate bar width (since it is not automatically done by QCustomPlot).
void PlotDock::adjustBars()
{
    const double padding = 0.15;
    int plottableCount = ui->plotWidget->plottableCount();

    if (plottableCount == 0)
        return;

    const double groupedWidth = 1.0 / plottableCount;
    QCPBars* previousBar = nullptr;
    QCPBarsGroup* barsGroup = m_stackedBars ? nullptr : new QCPBarsGroup(ui->plotWidget);
    for (int i = 0, ie = plottableCount; i < ie; ++i)
    {
        QCPBars* bar = qobject_cast<QCPBars*>(ui->plotWidget->plottable(i));
        if (bar) {
            if (m_stackedBars) {
                // Ungroup if grouped
                bar->setBarsGroup(nullptr);
                if (previousBar)
                    bar->moveAbove(previousBar);
                // Set width to ocuppy the full coordinate space, less padding
                bar->setWidth(1.0 - padding);
            } else {
                // Unstack if stacked
                bar->moveAbove(nullptr);
                bar->setBarsGroup(barsGroup);
                // Set width to a plot coordinate width, less padding
                bar->setWidth(groupedWidth - padding);
            }
            previousBar = bar;
        }
    }
}

namespace {
    void adjustOneAxisFormat (QCPAxis* axis, bool fixedFormat) {
        const QString format = fixedFormat? "f" : "gb";
        axis->setNumberFormat(format);
        int precision;
        // Different values for big numbers and small numbers.
        if (axis->range().center() > 999 && axis->range().size() > 10)
            precision = fixedFormat? 0 : 2;
        else
            precision = fixedFormat? 2 : 6;
        axis->setNumberPrecision(precision);
    }
}

void PlotDock::adjustAxisFormat()
{
    adjustOneAxisFormat(ui->plotWidget->xAxis, m_fixedFormat);
    adjustOneAxisFormat(ui->plotWidget->yAxis, m_fixedFormat);
    adjustOneAxisFormat(ui->plotWidget->yAxis2, m_fixedFormat);
}

void PlotDock::toggleStackedBars(bool stacked)
{
    m_stackedBars = stacked;
    adjustBars();
    ui->plotWidget->replot();
}

void PlotDock::reject()
{
    // We override this, to ensure the Escape key doesn't make this dialog
    // dock go away
    return;
}

void PlotDock::openPrintDialog()
{
    QPrinter printer;
    QPrintPreviewDialog previewDialog(&printer, this);
    connect(&previewDialog, &QPrintPreviewDialog::paintRequested, this, &PlotDock::renderPlot);
    previewDialog.exec();
}

void PlotDock::renderPlot(QPrinter* printer)
{
    QCPPainter painter(printer);
    QRectF pageRect = printer->pageRect(QPrinter::DevicePixel);

    int plotWidth = ui->plotWidget->viewport().width();
    int plotHeight = ui->plotWidget->viewport().height();
    double scale = pageRect.width()/static_cast<double>(plotWidth);

    painter.setMode(QCPPainter::pmVectorized);
    painter.setMode(QCPPainter::pmNoCaching);

    painter.scale(scale, scale);
    ui->plotWidget->toPainter(&painter, plotWidth, plotHeight);
}
